// SPDX-License-Identifier: proprietary
// temp_directory.hpp -- a fresh directory under the system temp directory that is removed, with everything in it, when the object goes (tests
// only). The operator tool's tests (D5 slice R3b-2) put their logs, key files and evidence directories in one, so a failed test cannot leave a
// file behind that a later run mistakes for its own.

#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace hy::test_helpers {

struct TempDirectory {
    std::filesystem::path path;

    TempDirectory() {
        static int counter = 0;
        std::error_code ec;
        path = std::filesystem::temp_directory_path(ec) /
               ("hy_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" + std::to_string(++counter));
        std::filesystem::create_directories(path, ec);
    }

    ~TempDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    // The path of `name` inside the directory, as a string.
    std::string file(std::string_view name) const { return (path / std::string(name)).string(); }
};

}  // namespace hy::test_helpers
