// KekLoader unit tests — L3 secure KEK file loader (Phase 0, 轨道 key-rotation
// substrate). Mirrors test_env_loader.cpp's fixture/coverage pattern for
// SecureEnvLoader, adapted for a fixed-size 32-byte binary KEK instead of
// .env key=value text. Synthetic test KEKs only.

#include <gtest/gtest.h>
#include <hengyuan/kek_loader.hpp>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#ifdef __linux__
#include <sys/stat.h>
#include <unistd.h>
#endif

using hy::kKekSize;
using hy::KekLoader;
using hy::KekLoadStatus;

namespace {

std::string make_synthetic_kek(std::uint8_t fill = 0x42) {
    return std::string(kKekSize, static_cast<char>(fill));
}

}  // namespace

class KekLoaderTest : public ::testing::Test {
protected:
    std::string tmp_path_;

    void SetUp() override {
#ifdef _WIN32
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        tmp_path_ = std::string(tmp) + "hy_test_kek_" +
                    std::to_string(GetCurrentProcessId()) + ".bin";
#else
        tmp_path_ = "/tmp/hy_test_kek_" + std::to_string(getpid()) + ".bin";
#endif
    }

    void TearDown() override { std::remove(tmp_path_.c_str()); }

    void write_file(const std::string& content) {
        std::ofstream f(tmp_path_, std::ios::binary);
        f << content;
        f.close();

#ifdef __linux__
        chmod(tmp_path_.c_str(), 0600);
#endif
    }
};

TEST_F(KekLoaderTest, LoadsValidKek) {
    const std::string kek_bytes = make_synthetic_kek(0x42);
    write_file(kek_bytes);

    KekLoader loader;
    auto status = loader.load(tmp_path_.c_str());
    EXPECT_EQ(status, KekLoadStatus::Ok);
    ASSERT_TRUE(loader.is_loaded());

    auto span = loader.get();
    ASSERT_EQ(span.size(), kKekSize);
    for (std::byte b : span) {
        EXPECT_EQ(b, std::byte{0x42});
    }
}

TEST_F(KekLoaderTest, FileNotFound) {
    KekLoader loader;
    auto status = loader.load("/nonexistent/path/to/kek.bin");
    EXPECT_EQ(status, KekLoadStatus::FileNotFound);
    EXPECT_FALSE(loader.is_loaded());
}

TEST_F(KekLoaderTest, RejectsTooSmallFile) {
    write_file(std::string(kKekSize - 1, 'x'));

    KekLoader loader;
    auto status = loader.load(tmp_path_.c_str());
    EXPECT_EQ(status, KekLoadStatus::SizeMismatch);
    EXPECT_FALSE(loader.is_loaded());
}

TEST_F(KekLoaderTest, RejectsTooLargeFile) {
    write_file(std::string(kKekSize + 1, 'x'));

    KekLoader loader;
    auto status = loader.load(tmp_path_.c_str());
    EXPECT_EQ(status, KekLoadStatus::SizeMismatch);
    EXPECT_FALSE(loader.is_loaded());
}

TEST_F(KekLoaderTest, RejectsEmptyFile) {
    write_file("");

    KekLoader loader;
    auto status = loader.load(tmp_path_.c_str());
    EXPECT_EQ(status, KekLoadStatus::SizeMismatch);
    EXPECT_FALSE(loader.is_loaded());
}

TEST_F(KekLoaderTest, WipeClearsData) {
    write_file(make_synthetic_kek(0x77));

    KekLoader loader;
    ASSERT_EQ(loader.load(tmp_path_.c_str()), KekLoadStatus::Ok);
    ASSERT_TRUE(loader.is_loaded());

    loader.wipe();
    EXPECT_FALSE(loader.is_loaded());
    for (std::byte b : loader.get()) {
        EXPECT_EQ(b, std::byte{0});
    }
}

TEST_F(KekLoaderTest, DestructorWipes) {
    write_file(make_synthetic_kek(0x99));

    {
        KekLoader loader;
        ASSERT_EQ(loader.load(tmp_path_.c_str()), KekLoadStatus::Ok);
        // loader goes out of scope -- destructor calls wipe()
    }
    SUCCEED();  // if we got here without crash, destruction succeeded
}

TEST_F(KekLoaderTest, ReloadWipesPrevious) {
    write_file(make_synthetic_kek(0x11));

    KekLoader loader;
    ASSERT_EQ(loader.load(tmp_path_.c_str()), KekLoadStatus::Ok);
    EXPECT_EQ(loader.get()[0], std::byte{0x11});

    write_file(make_synthetic_kek(0x22));
    ASSERT_EQ(loader.load(tmp_path_.c_str()), KekLoadStatus::Ok);
    EXPECT_EQ(loader.get()[0], std::byte{0x22});
}

TEST_F(KekLoaderTest, FailedReloadWipesPreviouslyLoadedKek) {
    write_file(make_synthetic_kek(0x33));

    KekLoader loader;
    ASSERT_EQ(loader.load(tmp_path_.c_str()), KekLoadStatus::Ok);
    ASSERT_TRUE(loader.is_loaded());

    // Second load targets a bad path -- the first KEK must not remain
    // readable after a failed reload attempt.
    auto status = loader.load("/nonexistent/path/to/kek.bin");
    EXPECT_EQ(status, KekLoadStatus::FileNotFound);
    EXPECT_FALSE(loader.is_loaded());
    for (std::byte b : loader.get()) {
        EXPECT_EQ(b, std::byte{0});
    }
}

#ifdef __linux__
TEST_F(KekLoaderTest, RejectsGroupReadable) {
    write_file(make_synthetic_kek());
    chmod(tmp_path_.c_str(), 0640);

    KekLoader loader;
    auto status = loader.load(tmp_path_.c_str());
    EXPECT_EQ(status, KekLoadStatus::PermissionTooWide);
    EXPECT_FALSE(loader.is_loaded());
}

TEST_F(KekLoaderTest, RejectsOtherReadable) {
    write_file(make_synthetic_kek());
    chmod(tmp_path_.c_str(), 0604);

    KekLoader loader;
    auto status = loader.load(tmp_path_.c_str());
    EXPECT_EQ(status, KekLoadStatus::PermissionTooWide);
    EXPECT_FALSE(loader.is_loaded());
}

TEST_F(KekLoaderTest, RejectsWorldReadable) {
    write_file(make_synthetic_kek());
    chmod(tmp_path_.c_str(), 0644);

    KekLoader loader;
    auto status = loader.load(tmp_path_.c_str());
    EXPECT_EQ(status, KekLoadStatus::PermissionTooWide);
    EXPECT_FALSE(loader.is_loaded());
}

TEST_F(KekLoaderTest, Accepts0400ReadOnly) {
    write_file(make_synthetic_kek(0x55));
    chmod(tmp_path_.c_str(), 0400);

    KekLoader loader;
    auto status = loader.load(tmp_path_.c_str());
    EXPECT_EQ(status, KekLoadStatus::Ok);
    EXPECT_EQ(loader.get()[0], std::byte{0x55});
}

TEST_F(KekLoaderTest, RejectsSymlink) {
    write_file(make_synthetic_kek());
    std::string link_path = tmp_path_ + ".link";
    ASSERT_EQ(symlink(tmp_path_.c_str(), link_path.c_str()), 0)
        << "test fixture setup failed: " << strerror(errno);

    KekLoader loader;
    auto status = loader.load(link_path.c_str());
    EXPECT_EQ(status, KekLoadStatus::IsSymlink);
    EXPECT_FALSE(loader.is_loaded());

    unlink(link_path.c_str());
}
#endif
