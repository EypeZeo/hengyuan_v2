// KekLoader unit tests — L3 secure KEK file loader (Phase 0, 轨道 key-rotation
// substrate). Mirrors test_env_loader.cpp's fixture/coverage pattern for
// SecureEnvLoader, adapted for a fixed-size 32-byte binary KEK instead of
// .env key=value text. Synthetic test KEKs only.

#include <gtest/gtest.h>
#include <hengyuan/kek_loader.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <thread>

#ifdef __linux__
#include <fcntl.h>
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

TEST_F(KekLoaderTest, RejectsDanglingSymlink) {
    // The target does not exist, so a following open would say "not found"; the
    // trailing symlink itself must still be what gets reported.
    std::string link_path = tmp_path_ + ".link";
    ASSERT_EQ(symlink((tmp_path_ + ".absent").c_str(), link_path.c_str()), 0)
        << "test fixture setup failed: " << strerror(errno);

    KekLoader loader;
    auto status = loader.load(link_path.c_str());
    EXPECT_EQ(status, KekLoadStatus::IsSymlink);
    EXPECT_FALSE(loader.is_loaded());

    unlink(link_path.c_str());
}

TEST_F(KekLoaderTest, RejectsDirectory) {
    ASSERT_EQ(mkdir(tmp_path_.c_str(), 0700), 0)
        << "test fixture setup failed: " << strerror(errno);

    KekLoader loader;
    auto status = loader.load(tmp_path_.c_str());
    EXPECT_EQ(status, KekLoadStatus::FileNotFound);
    EXPECT_FALSE(loader.is_loaded());
}

TEST_F(KekLoaderTest, RejectsFifoWithoutBlocking) {
    // open(2) on a FIFO parks until a writer appears unless O_NONBLOCK is given,
    // and nothing here will ever write: a FIFO swapped in at the path must be
    // turned away, not waited on.
    ASSERT_EQ(mkfifo(tmp_path_.c_str(), 0600), 0)
        << "test fixture setup failed: " << strerror(errno);

    auto status = std::async(std::launch::async, [this] {
        KekLoader loader;
        return loader.load(tmp_path_.c_str());
    });

    if (status.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
        // load() is parked in a blocking open(). Release it by becoming the writer
        // (a parked reader counts, so this non-blocking open succeeds): the test
        // then fails below instead of hanging the whole run.
        const int writer = open(tmp_path_.c_str(), O_WRONLY | O_NONBLOCK);
        ADD_FAILURE() << "KekLoader::load blocked on a FIFO";
        if (writer >= 0) close(writer);
    }
    EXPECT_EQ(status.get(), KekLoadStatus::FileNotFound);
}

TEST_F(KekLoaderTest, MapsPathResolutionErrorsToFileNotFound) {
    write_file(make_synthetic_kek());
    KekLoader loader;

    // ENOTDIR: the path runs through a regular file
    EXPECT_EQ(loader.load((tmp_path_ + "/nested").c_str()), KekLoadStatus::FileNotFound);

    // ENAMETOOLONG
    const std::string too_long = "/tmp/" + std::string(5000, 'a');
    EXPECT_EQ(loader.load(too_long.c_str()), KekLoadStatus::FileNotFound);
}

namespace {

// Loads attempted against the swapping path in SwapRaceNeverLoadsAnUncheckedFile;
// its 5 s deadline bounds the run on a slow host. Against the old lstat-then-open
// loader the first anomaly came within ~200 loads on a multi-core host, and within
// this cap in most runs pinned to a single CPU.
constexpr int kSwapRaceLoads = 50000;

// Descriptors currently open in this process. The one the directory iterator
// itself holds is present in every count, so it cancels out of comparisons.
std::size_t open_fd_count() {
    std::size_t n = 0;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        (void)entry;
        ++n;
    }
    return n;
}

}  // namespace

TEST_F(KekLoaderTest, ReleasesDescriptorOnEveryPath) {
    // load() holds one descriptor across the whole gate chain; every exit,
    // accepted or rejected, must give it back.
    const std::string link_path = tmp_path_ + ".link";

    auto exercise_every_exit = [&] {
        KekLoader loader;
        const char* path = tmp_path_.c_str();

        write_file(make_synthetic_kek());
        EXPECT_EQ(loader.load(path), KekLoadStatus::Ok);

        write_file(std::string(kKekSize - 1, 'x'));
        EXPECT_EQ(loader.load(path), KekLoadStatus::SizeMismatch);

        write_file(make_synthetic_kek());
        chmod(path, 0644);
        EXPECT_EQ(loader.load(path), KekLoadStatus::PermissionTooWide);

        std::remove(path);
        EXPECT_EQ(mkdir(path, 0700), 0);
        EXPECT_EQ(loader.load(path), KekLoadStatus::FileNotFound);
        EXPECT_EQ(rmdir(path), 0);

        EXPECT_EQ(symlink("/nonexistent_hy_target", link_path.c_str()), 0);
        EXPECT_EQ(loader.load(link_path.c_str()), KekLoadStatus::IsSymlink);
        unlink(link_path.c_str());
    };

    exercise_every_exit();  // warm-up: one-time lazy allocations must not count
    const std::size_t before = open_fd_count();
    exercise_every_exit();
    EXPECT_EQ(open_fd_count(), before);
}

TEST_F(KekLoaderTest, SwapRaceNeverLoadsAnUncheckedFile) {
    // The path is flipped between the legitimate 0600 KEK, "absent" and a
    // world-readable decoy while load() runs. Whatever the interleaving, a load
    // that succeeds must have read the legitimate file: a gate evaluated on one
    // file and a read taken from another is the check-then-use hole itself.
    const std::string legit = tmp_path_ + ".legit";
    const std::string decoy = tmp_path_ + ".decoy";
    struct RemoveOnExit {
        const std::string& path;
        ~RemoveOnExit() { std::remove(path.c_str()); }
    } remove_decoy{decoy};

    write_file(make_synthetic_kek(0x42));
    {
        std::ofstream d(decoy, std::ios::binary);
        d << make_synthetic_kek(0x99);
    }
    chmod(decoy.c_str(), 0644);  // would fail the permission gate -- if evaluated on it

    {
        // fixture check: with the path stable, the legitimate KEK loads
        KekLoader loader;
        ASSERT_EQ(loader.load(tmp_path_.c_str()), KekLoadStatus::Ok);
        ASSERT_EQ(loader.get()[0], std::byte{0x42});
    }

    std::atomic<bool> stop{false};
    std::thread swapper([&] {
        // path: legit -> absent -> decoy -> absent -> legit -> ...
        while (!stop.load(std::memory_order_relaxed)) {
            rename(tmp_path_.c_str(), legit.c_str());
            rename(decoy.c_str(), tmp_path_.c_str());
            rename(tmp_path_.c_str(), decoy.c_str());
            rename(legit.c_str(), tmp_path_.c_str());
        }
    });

    KekLoader loader;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (int i = 0; i < kSwapRaceLoads && std::chrono::steady_clock::now() < deadline; ++i) {
        const auto status = loader.load(tmp_path_.c_str());
        if (status == KekLoadStatus::Ok) {
            bool all_legit = true;
            for (std::byte b : loader.get()) {
                all_legit = all_legit && b == std::byte{0x42};
            }
            if (!all_legit) {
                ADD_FAILURE() << "load #" << i << " succeeded on the decoy";
                break;
            }
        } else if (status != KekLoadStatus::FileNotFound &&
                   status != KekLoadStatus::PermissionTooWide) {
            ADD_FAILURE() << "load #" << i << ": unexpected status " << static_cast<int>(status);
            break;
        }
    }

    stop.store(true, std::memory_order_relaxed);
    swapper.join();
}
#endif
