// Windows-only real-filesystem tests for windows_native_io.hpp -- NTSTATUS
// error paths, sharing-violation detection, reparse-point rejection, and the
// relative-name traversal/reserved-device-name defense-in-depth layer.
// Governance: L2 (real file I/O). No WSL2/POSIX equivalent -- this whole
// header is #ifdef _WIN32-only (see its own file header comment).
#ifdef _WIN32

#include <gtest/gtest.h>
#include <hengyuan/windows_native_io.hpp>
#include "test_helpers/windows_junction.hpp"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>  // FSCTL_SET_REPARSE_POINT -- not always pulled in by windows.h alone

using namespace hy::win_native;

namespace {

std::filesystem::path make_temp_dir(std::string_view tag) {
    auto dir = std::filesystem::temp_directory_path() /
               ("hy_win_native_io_" + std::string(tag) + "_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    return dir;
}

// The byte-exact junction builder is shared with test_compaction_lease.cpp.
using hy::test_helpers::create_directory_junction;
}  // namespace

// ===========================================================================
// is_relative_name_traversal_safe: pure-function unit tests
// ===========================================================================

TEST(RelativeNameTraversalSafe, RejectsEmpty) {
    EXPECT_FALSE(is_relative_name_traversal_safe(L""));
}
TEST(RelativeNameTraversalSafe, RejectsDotAndDotDot) {
    EXPECT_FALSE(is_relative_name_traversal_safe(L"."));
    EXPECT_FALSE(is_relative_name_traversal_safe(L".."));
}
TEST(RelativeNameTraversalSafe, RejectsForwardSlash) {
    EXPECT_FALSE(is_relative_name_traversal_safe(L"a/b"));
}
TEST(RelativeNameTraversalSafe, RejectsBackslash) {
    EXPECT_FALSE(is_relative_name_traversal_safe(L"a\\b"));
}
TEST(RelativeNameTraversalSafe, RejectsEmbeddedNul) {
    EXPECT_FALSE(is_relative_name_traversal_safe(std::wstring(L"a\0b", 3)));
}
TEST(RelativeNameTraversalSafe, RejectsReservedDeviceNamesCaseInsensitive) {
    EXPECT_FALSE(is_relative_name_traversal_safe(L"CON"));
    EXPECT_FALSE(is_relative_name_traversal_safe(L"con"));
    EXPECT_FALSE(is_relative_name_traversal_safe(L"PRN"));
    EXPECT_FALSE(is_relative_name_traversal_safe(L"AUX"));
    EXPECT_FALSE(is_relative_name_traversal_safe(L"NUL"));
    EXPECT_FALSE(is_relative_name_traversal_safe(L"COM1"));
    EXPECT_FALSE(is_relative_name_traversal_safe(L"com9"));
    EXPECT_FALSE(is_relative_name_traversal_safe(L"LPT1"));
    EXPECT_FALSE(is_relative_name_traversal_safe(L"lpt9"));
}
TEST(RelativeNameTraversalSafe, RejectsReservedDeviceNameWithExtension) {
    EXPECT_FALSE(is_relative_name_traversal_safe(L"NUL.txt"));
    EXPECT_FALSE(is_relative_name_traversal_safe(L"con.compaction-candidate-intent"));
}
TEST(RelativeNameTraversalSafe, AcceptsNonReservedLookalikes) {
    // COM10/LPT10/CONFIG/NULL are NOT reserved -- only an exact CONx/LPTx
    // (x in 1..9) or CON/PRN/AUX/NUL base name is.
    EXPECT_TRUE(is_relative_name_traversal_safe(L"COM10"));
    EXPECT_TRUE(is_relative_name_traversal_safe(L"LPT10"));
    EXPECT_TRUE(is_relative_name_traversal_safe(L"CONFIG"));
    EXPECT_TRUE(is_relative_name_traversal_safe(L"NULL"));
}
TEST(RelativeNameTraversalSafe, AcceptsRealRoundDNames) {
    EXPECT_TRUE(is_relative_name_traversal_safe(L"compaction-candidate-intent"));
    EXPECT_TRUE(is_relative_name_traversal_safe(L"compaction-candidate.lock"));
    EXPECT_TRUE(is_relative_name_traversal_safe(L"compaction-intent-x-0000000000000001-0000000000000001.x1"));
}

// ===========================================================================
// The check is actually wired into the three open-relative functions, not
// just defined -- these catch a "forgot to call it" regression the pure
// unit tests above cannot.
// ===========================================================================

TEST(WindowsNativeIoTraversalWiring, CreateNewRelativeRejectsSeparator) {
    auto dir = make_temp_dir("create_traversal");
    RawHandle dir_handle;
    ASSERT_EQ(open_directory(dir.wstring(), dir_handle), DirOpenResult::Opened);

    RawHandle out;
    EXPECT_EQ(create_new_relative(dir_handle, L"../escape", out), RelativeCreateResult::Failed);
    EXPECT_EQ(create_new_relative(dir_handle, L"NUL", out), RelativeCreateResult::Failed);

    dir_handle.reset();
    std::filesystem::remove_all(dir);
}

TEST(WindowsNativeIoTraversalWiring, OpenExistingRelativeRejectsSeparator) {
    auto dir = make_temp_dir("open_traversal");
    RawHandle dir_handle;
    ASSERT_EQ(open_directory(dir.wstring(), dir_handle), DirOpenResult::Opened);

    RawHandle out;
    EXPECT_EQ(open_existing_relative(dir_handle, L"..\\escape", out), RelativeOpenResult::Failed);
    EXPECT_EQ(open_existing_relative(dir_handle, L"CON.txt", out), RelativeOpenResult::Failed);

    dir_handle.reset();
    std::filesystem::remove_all(dir);
}

TEST(WindowsNativeIoTraversalWiring, ExclusiveRelativeRejectsSeparator) {
    auto dir = make_temp_dir("lock_traversal");
    RawHandle dir_handle;
    ASSERT_EQ(open_directory(dir.wstring(), dir_handle), DirOpenResult::Opened);

    RawHandle out;
    EXPECT_EQ(open_or_create_exclusive_relative(dir_handle, L"a/b", out), ExclusiveLockResult::Failed);

    dir_handle.reset();
    std::filesystem::remove_all(dir);
}

// ===========================================================================
// open_directory: NotFound / Opened / RejectedReparsePoint
// ===========================================================================

TEST(OpenDirectory, NotFoundOnMissingPath) {
    auto dir = make_temp_dir("missing") / "does_not_exist";
    RawHandle h;
    EXPECT_EQ(open_directory(dir.wstring(), h), DirOpenResult::NotFound);
}

TEST(OpenDirectory, OpenedOnRealDirectory) {
    auto dir = make_temp_dir("real");
    RawHandle h;
    EXPECT_EQ(open_directory(dir.wstring(), h), DirOpenResult::Opened);
    EXPECT_TRUE(h.valid());
    h.reset();
    std::filesystem::remove_all(dir);
}

TEST(OpenDirectory, RejectsDirectoryJunction) {
    auto parent = make_temp_dir("junction_parent");
    auto real_target = parent / "real_target";
    std::filesystem::create_directories(real_target);
    auto link = parent / "link";

    if (!create_directory_junction(link, real_target)) {
        GTEST_SKIP() << "could not create a directory junction on this filesystem/runner -- "
                        "reparse-point rejection cannot be exercised here";
    }

    RawHandle h;
    EXPECT_EQ(open_directory(link.wstring(), h), DirOpenResult::RejectedReparsePoint);

    // Cleanup: RemoveDirectoryW on the junction's own path removes the
    // reparse point without touching (or recursing into) real_target --
    // std::filesystem::remove_all could behave surprisingly on a reparse
    // point depending on implementation, so this is deliberately explicit.
    ::RemoveDirectoryW(link.wstring().c_str());
    std::filesystem::remove_all(parent);
}

// ===========================================================================
// open_or_create_exclusive_relative: Acquired / HeldElsewhere
// ===========================================================================

TEST(ExclusiveRelative, AcquiredOnFreshFile) {
    auto dir = make_temp_dir("excl_fresh");
    RawHandle dir_handle;
    ASSERT_EQ(open_directory(dir.wstring(), dir_handle), DirOpenResult::Opened);

    RawHandle out;
    EXPECT_EQ(open_or_create_exclusive_relative(dir_handle, L"lockfile", out), ExclusiveLockResult::Acquired);

    out.reset();
    dir_handle.reset();
    std::filesystem::remove_all(dir);
}

TEST(ExclusiveRelative, HeldElsewhereWhenAlreadyOpenExclusively) {
    auto dir = make_temp_dir("excl_conflict");
    RawHandle dir_handle;
    ASSERT_EQ(open_directory(dir.wstring(), dir_handle), DirOpenResult::Opened);

    RawHandle first;
    ASSERT_EQ(open_or_create_exclusive_relative(dir_handle, L"lockfile", first), ExclusiveLockResult::Acquired);

    RawHandle second;
    EXPECT_EQ(open_or_create_exclusive_relative(dir_handle, L"lockfile", second), ExclusiveLockResult::HeldElsewhere);

    first.reset();
    dir_handle.reset();
    std::filesystem::remove_all(dir);
}

// ===========================================================================
// create_new_relative / rename_no_replace: AlreadyExists / Renamed
// ===========================================================================

TEST(CreateNewRelative, AlreadyExistsOnSecondCreate) {
    auto dir = make_temp_dir("create_exists");
    RawHandle dir_handle;
    ASSERT_EQ(open_directory(dir.wstring(), dir_handle), DirOpenResult::Opened);

    RawHandle first;
    ASSERT_EQ(create_new_relative(dir_handle, L"target", first), RelativeCreateResult::Created);
    first.reset();

    RawHandle second;
    EXPECT_EQ(create_new_relative(dir_handle, L"target", second), RelativeCreateResult::AlreadyExists);

    dir_handle.reset();
    std::filesystem::remove_all(dir);
}

TEST(RenameNoReplace, AlreadyExistsWhenTargetPresent) {
    auto dir = make_temp_dir("rename_exists");
    RawHandle dir_handle;
    ASSERT_EQ(open_directory(dir.wstring(), dir_handle), DirOpenResult::Opened);

    RawHandle existing;
    ASSERT_EQ(create_new_relative(dir_handle, L"final", existing), RelativeCreateResult::Created);
    existing.reset();

    RawHandle tmp;
    ASSERT_EQ(create_new_relative(dir_handle, L"tmp", tmp), RelativeCreateResult::Created);

    NTSTATUS status = 0;
    EXPECT_EQ(rename_no_replace(tmp, dir_handle, L"final", &status), RelativeRenameResult::AlreadyExists);

    tmp.reset();
    dir_handle.reset();
    std::filesystem::remove_all(dir);
}

#endif  // _WIN32
