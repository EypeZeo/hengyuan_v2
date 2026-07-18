// P2-EXEC-LIVE-01 D12-2: SecureEnvLoader unit tests — L3 secure .env file loader.
// Uses synthetic test files only. Real API keys never appear in test code.
#include <gtest/gtest.h>
#include <hengyuan/env_loader.hpp>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#ifdef __linux__
#include <sys/stat.h>
#include <unistd.h>
#endif

using hy::EnvAllowlist;
using hy::EnvLoadStatus;
using hy::SecureEnvLoader;

static constexpr std::string_view kAllowedKeys[] = {
    "BINANCE_API_KEY",
    "BINANCE_API_SECRET",
    "MAX_NOTIONAL_USD",
};

static constexpr EnvAllowlist kAllowlist{kAllowedKeys, 3};

class EnvLoaderTest : public ::testing::Test {
protected:
    std::string tmp_path_;

    void SetUp() override {
        // Create a unique temp file path
#ifdef _WIN32
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        tmp_path_ = std::string(tmp) + "hy_test_env_" +
                     std::to_string(GetCurrentProcessId()) + ".env";
#else
        tmp_path_ = "/tmp/hy_test_env_" + std::to_string(getpid()) + ".env";
#endif
    }

    void TearDown() override {
        std::remove(tmp_path_.c_str());
    }

    void write_file(const std::string& content) {
        std::ofstream f(tmp_path_, std::ios::binary);
        f << content;
        f.close();

#ifdef __linux__
        // Set proper 0600 permissions so the loader accepts it
        chmod(tmp_path_.c_str(), 0600);
#endif
    }
};

TEST_F(EnvLoaderTest, LoadsValidFile) {
    write_file("BINANCE_API_KEY=synthetic_test_key_123\n"
               "BINANCE_API_SECRET=synthetic_test_secret_456\n");

    SecureEnvLoader loader;
    auto r = loader.load(tmp_path_.c_str(), kAllowlist);
    EXPECT_EQ(r.status, EnvLoadStatus::Ok);
    EXPECT_TRUE(loader.is_loaded());
    EXPECT_EQ(loader.get("BINANCE_API_KEY"), "synthetic_test_key_123");
    EXPECT_EQ(loader.get("BINANCE_API_SECRET"), "synthetic_test_secret_456");
}

TEST_F(EnvLoaderTest, FileNotFound) {
    SecureEnvLoader loader;
    auto r = loader.load("/nonexistent/path/to/.env", kAllowlist);
    EXPECT_EQ(r.status, EnvLoadStatus::FileNotFound);
    EXPECT_FALSE(loader.is_loaded());
}

TEST_F(EnvLoaderTest, ParseErrorPropagates) {
    write_file("UNKNOWN_KEY=val\n");

    SecureEnvLoader loader;
    auto r = loader.load(tmp_path_.c_str(), kAllowlist);
    EXPECT_EQ(r.status, EnvLoadStatus::ParseError);
    EXPECT_FALSE(loader.is_loaded());
}

TEST_F(EnvLoaderTest, WipeClearsData) {
    write_file("BINANCE_API_KEY=wipe_test_value\n");

    SecureEnvLoader loader;
    auto r = loader.load(tmp_path_.c_str(), kAllowlist);
    EXPECT_EQ(r.status, EnvLoadStatus::Ok);
    EXPECT_EQ(loader.get("BINANCE_API_KEY"), "wipe_test_value");

    loader.wipe();
    EXPECT_FALSE(loader.is_loaded());
    EXPECT_TRUE(loader.get("BINANCE_API_KEY").empty());
}

TEST_F(EnvLoaderTest, DestructorWipes) {
    write_file("BINANCE_API_KEY=destructor_test\n");

    {
        SecureEnvLoader loader;
        auto r = loader.load(tmp_path_.c_str(), kAllowlist);
        EXPECT_EQ(r.status, EnvLoadStatus::Ok);
        // loader goes out of scope — destructor calls wipe()
    }
    // If we got here without crash, destruction succeeded
    SUCCEED();
}

TEST_F(EnvLoaderTest, ReloadWipesPrevious) {
    write_file("BINANCE_API_KEY=first_load\n");

    SecureEnvLoader loader;
    auto r1 = loader.load(tmp_path_.c_str(), kAllowlist);
    EXPECT_EQ(r1.status, EnvLoadStatus::Ok);
    EXPECT_EQ(loader.get("BINANCE_API_KEY"), "first_load");

    // Overwrite file and reload
    write_file("BINANCE_API_KEY=second_load\n");
    auto r2 = loader.load(tmp_path_.c_str(), kAllowlist);
    EXPECT_EQ(r2.status, EnvLoadStatus::Ok);
    EXPECT_EQ(loader.get("BINANCE_API_KEY"), "second_load");
}

TEST_F(EnvLoaderTest, HasReturnsFalseWhenNotLoaded) {
    SecureEnvLoader loader;
    EXPECT_FALSE(loader.has("BINANCE_API_KEY"));
    EXPECT_TRUE(loader.get("BINANCE_API_KEY").empty());
}

TEST_F(EnvLoaderTest, EmptyFileIsOk) {
    write_file("");

    SecureEnvLoader loader;
    auto r = loader.load(tmp_path_.c_str(), kAllowlist);
    EXPECT_EQ(r.status, EnvLoadStatus::Ok);
    EXPECT_TRUE(loader.is_loaded());
    EXPECT_FALSE(loader.has("BINANCE_API_KEY"));
}

TEST_F(EnvLoaderTest, WindowsLineEndingsWork) {
    write_file("BINANCE_API_KEY=crlf_test\r\nBINANCE_API_SECRET=crlf_secret\r\n");

    SecureEnvLoader loader;
    auto r = loader.load(tmp_path_.c_str(), kAllowlist);
    EXPECT_EQ(r.status, EnvLoadStatus::Ok);
    EXPECT_EQ(loader.get("BINANCE_API_KEY"), "crlf_test");
    EXPECT_EQ(loader.get("BINANCE_API_SECRET"), "crlf_secret");
}

#ifdef __linux__
TEST_F(EnvLoaderTest, RejectsGroupReadable) {
    write_file("BINANCE_API_KEY=perm_test\n");
    chmod(tmp_path_.c_str(), 0640);  // group readable

    SecureEnvLoader loader;
    auto r = loader.load(tmp_path_.c_str(), kAllowlist);
    EXPECT_EQ(r.status, EnvLoadStatus::PermissionTooWide);
    EXPECT_FALSE(loader.is_loaded());
}

TEST_F(EnvLoaderTest, RejectsOtherReadable) {
    write_file("BINANCE_API_KEY=perm_test\n");
    chmod(tmp_path_.c_str(), 0604);  // other readable

    SecureEnvLoader loader;
    auto r = loader.load(tmp_path_.c_str(), kAllowlist);
    EXPECT_EQ(r.status, EnvLoadStatus::PermissionTooWide);
    EXPECT_FALSE(loader.is_loaded());
}

TEST_F(EnvLoaderTest, RejectsWorldReadable) {
    write_file("BINANCE_API_KEY=perm_test\n");
    chmod(tmp_path_.c_str(), 0644);

    SecureEnvLoader loader;
    auto r = loader.load(tmp_path_.c_str(), kAllowlist);
    EXPECT_EQ(r.status, EnvLoadStatus::PermissionTooWide);
    EXPECT_FALSE(loader.is_loaded());
}

TEST_F(EnvLoaderTest, Accepts0400ReadOnly) {
    write_file("BINANCE_API_KEY=readonly_test\n");
    chmod(tmp_path_.c_str(), 0400);

    SecureEnvLoader loader;
    auto r = loader.load(tmp_path_.c_str(), kAllowlist);
    EXPECT_EQ(r.status, EnvLoadStatus::Ok);
    EXPECT_EQ(loader.get("BINANCE_API_KEY"), "readonly_test");
}

TEST_F(EnvLoaderTest, RejectsSymlink) {
    write_file("BINANCE_API_KEY=symlink_target\n");
    std::string link_path = tmp_path_ + ".link";
    // symlink() is warn_unused_result on glibc; if fixture setup itself fails,
    // fail loudly here rather than silently testing the loader against a
    // non-symlink path (which would make this test pass for the wrong reason).
    ASSERT_EQ(symlink(tmp_path_.c_str(), link_path.c_str()), 0)
        << "test fixture setup failed: " << strerror(errno);

    SecureEnvLoader loader;
    auto r = loader.load(link_path.c_str(), kAllowlist);
    EXPECT_EQ(r.status, EnvLoadStatus::IsSymlink);
    EXPECT_FALSE(loader.is_loaded());

    unlink(link_path.c_str());
}
#endif
