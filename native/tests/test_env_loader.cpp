// P2-EXEC-LIVE-01 D12-2: SecureEnvLoader unit tests — L3 secure .env file loader.
// Uses synthetic test files only. Real API keys never appear in test code.
#include <gtest/gtest.h>
#include <hengyuan/env_loader.hpp>

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

TEST_F(EnvLoaderTest, RejectsTooLargeFile) {
    write_file(std::string(hy::kMaxEnvFileSize + 1, 'x'));

    SecureEnvLoader loader;
    auto r = loader.load(tmp_path_.c_str(), kAllowlist);
    EXPECT_EQ(r.status, EnvLoadStatus::FileTooLarge);
    EXPECT_FALSE(loader.is_loaded());
}

TEST_F(EnvLoaderTest, AcceptsFileAtSizeLimit) {
    // "BINANCE_API_KEY=" + value + "\n", padded to exactly kMaxEnvFileSize bytes:
    // the limit is inclusive.
    const std::string key = "BINANCE_API_KEY=";
    const std::string value(hy::kMaxEnvFileSize - key.size() - 1, 'a');
    write_file(key + value + "\n");

    SecureEnvLoader loader;
    auto r = loader.load(tmp_path_.c_str(), kAllowlist);
    EXPECT_EQ(r.status, EnvLoadStatus::Ok);
    EXPECT_EQ(loader.get("BINANCE_API_KEY"), value);
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

TEST_F(EnvLoaderTest, RejectsDanglingSymlink) {
    // The target does not exist, so a following open would say "not found"; the
    // trailing symlink itself must still be what gets reported.
    std::string link_path = tmp_path_ + ".link";
    ASSERT_EQ(symlink((tmp_path_ + ".absent").c_str(), link_path.c_str()), 0)
        << "test fixture setup failed: " << strerror(errno);

    SecureEnvLoader loader;
    auto r = loader.load(link_path.c_str(), kAllowlist);
    EXPECT_EQ(r.status, EnvLoadStatus::IsSymlink);
    EXPECT_FALSE(loader.is_loaded());

    unlink(link_path.c_str());
}

TEST_F(EnvLoaderTest, RejectsDirectory) {
    ASSERT_EQ(mkdir(tmp_path_.c_str(), 0700), 0)
        << "test fixture setup failed: " << strerror(errno);

    SecureEnvLoader loader;
    auto r = loader.load(tmp_path_.c_str(), kAllowlist);
    EXPECT_EQ(r.status, EnvLoadStatus::FileNotFound);
    EXPECT_FALSE(loader.is_loaded());
}

TEST_F(EnvLoaderTest, RejectsFifoWithoutBlocking) {
    // open(2) on a FIFO parks until a writer appears unless O_NONBLOCK is given,
    // and nothing here will ever write: a FIFO swapped in at the path must be
    // turned away, not waited on.
    ASSERT_EQ(mkfifo(tmp_path_.c_str(), 0600), 0)
        << "test fixture setup failed: " << strerror(errno);

    auto status = std::async(std::launch::async, [this] {
        SecureEnvLoader loader;
        return loader.load(tmp_path_.c_str(), kAllowlist).status;
    });

    if (status.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
        // load() is parked in a blocking open(). Release it by becoming the writer
        // (a parked reader counts, so this non-blocking open succeeds): the test
        // then fails below instead of hanging the whole run.
        const int writer = open(tmp_path_.c_str(), O_WRONLY | O_NONBLOCK);
        ADD_FAILURE() << "SecureEnvLoader::load blocked on a FIFO";
        if (writer >= 0) close(writer);
    }
    EXPECT_EQ(status.get(), EnvLoadStatus::FileNotFound);
}

TEST_F(EnvLoaderTest, MapsPathResolutionErrorsToFileNotFound) {
    write_file("BINANCE_API_KEY=resolve_test\n");
    SecureEnvLoader loader;

    // ENOTDIR: the path runs through a regular file
    auto through_file = loader.load((tmp_path_ + "/nested").c_str(), kAllowlist);
    EXPECT_EQ(through_file.status, EnvLoadStatus::FileNotFound);

    // ENAMETOOLONG
    const std::string too_long = "/tmp/" + std::string(5000, 'a');
    auto name_too_long = loader.load(too_long.c_str(), kAllowlist);
    EXPECT_EQ(name_too_long.status, EnvLoadStatus::FileNotFound);
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

TEST_F(EnvLoaderTest, ReleasesDescriptorOnEveryPath) {
    // load() holds one descriptor across the whole gate chain; every exit,
    // accepted or rejected, must give it back.
    const std::string link_path = tmp_path_ + ".link";

    auto exercise_every_exit = [&] {
        SecureEnvLoader loader;
        const char* path = tmp_path_.c_str();

        write_file("BINANCE_API_KEY=fd_test\n");
        EXPECT_EQ(loader.load(path, kAllowlist).status, EnvLoadStatus::Ok);

        write_file("UNKNOWN_KEY=val\n");
        EXPECT_EQ(loader.load(path, kAllowlist).status, EnvLoadStatus::ParseError);

        write_file(std::string(hy::kMaxEnvFileSize + 1, 'x'));
        EXPECT_EQ(loader.load(path, kAllowlist).status, EnvLoadStatus::FileTooLarge);

        write_file("BINANCE_API_KEY=fd_test\n");
        chmod(path, 0644);
        EXPECT_EQ(loader.load(path, kAllowlist).status, EnvLoadStatus::PermissionTooWide);

        std::remove(path);
        EXPECT_EQ(mkdir(path, 0700), 0);
        EXPECT_EQ(loader.load(path, kAllowlist).status, EnvLoadStatus::FileNotFound);
        EXPECT_EQ(rmdir(path), 0);

        EXPECT_EQ(symlink("/nonexistent_hy_target", link_path.c_str()), 0);
        EXPECT_EQ(loader.load(link_path.c_str(), kAllowlist).status, EnvLoadStatus::IsSymlink);
        unlink(link_path.c_str());
    };

    exercise_every_exit();  // warm-up: one-time lazy allocations must not count
    const std::size_t before = open_fd_count();
    exercise_every_exit();
    EXPECT_EQ(open_fd_count(), before);
}

TEST_F(EnvLoaderTest, SwapRaceNeverLoadsAnUncheckedFile) {
    // The path is flipped between the legitimate 0600 file, "absent" and a
    // world-readable decoy while load() runs. Whatever the interleaving, a load
    // that succeeds must have read the legitimate file: a gate evaluated on one
    // file and a read taken from another is the check-then-use hole itself.
    const std::string legit = tmp_path_ + ".legit";
    const std::string decoy = tmp_path_ + ".decoy";
    struct RemoveOnExit {
        const std::string& path;
        ~RemoveOnExit() { std::remove(path.c_str()); }
    } remove_decoy{decoy};

    write_file("BINANCE_API_KEY=legit\n");
    {
        std::ofstream d(decoy, std::ios::binary);
        d << "BINANCE_API_KEY=decoy\n";
    }
    chmod(decoy.c_str(), 0644);  // would fail the permission gate -- if evaluated on it

    {
        // fixture check: with the path stable, the legitimate file loads
        SecureEnvLoader loader;
        ASSERT_EQ(loader.load(tmp_path_.c_str(), kAllowlist).status, EnvLoadStatus::Ok);
        ASSERT_EQ(loader.get("BINANCE_API_KEY"), "legit");
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

    SecureEnvLoader loader;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (int i = 0; i < kSwapRaceLoads && std::chrono::steady_clock::now() < deadline; ++i) {
        const auto r = loader.load(tmp_path_.c_str(), kAllowlist);
        if (r.status == EnvLoadStatus::Ok) {
            if (loader.get("BINANCE_API_KEY") != "legit") {
                ADD_FAILURE() << "load #" << i << " succeeded on the decoy";
                break;
            }
        } else if (r.status != EnvLoadStatus::FileNotFound &&
                   r.status != EnvLoadStatus::PermissionTooWide) {
            ADD_FAILURE() << "load #" << i << ": unexpected status "
                          << static_cast<int>(r.status);
            break;
        }
    }

    stop.store(true, std::memory_order_relaxed);
    swapper.join();
}
#endif
