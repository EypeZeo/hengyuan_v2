// SPDX-License-Identifier: proprietary
// bound_test_credentials.hpp — synthetic testnet credentials, bound to the CALLING thread, for tests that need a BoundHmacCredentials.
//
// The apiKey and secretKey are the ones Binance's own docs print for their signing examples (they are already in test_binance_signer.cpp);
// nothing here is, or looks like, a real credential. BoundHmacCredentials answers only on the thread that bound it (F11), so construct this on
// the thread that will sign — in a test, the test thread, which plays the role of the hot/submit thread.
//
// Same recipe as WsApiSigningTest::SetUp (test_binance_ws_api_signing.cpp): a temporary .env file with owner-only permissions, loaded by the
// real SecureEnvLoader, bound by the real BoundHmacCredentials::load_and_bind_credentials(); the file is removed in the destructor.

#pragma once

#include <hengyuan/binance_environment.hpp>

#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace hy::test_helpers {

class BoundTestCredentials {
public:
    static constexpr std::string_view kApiKey = "vmPUZE6mv9SD5VNHk4HlWFsOr6aKE2zvsw0MuIgwCIPy6utIco14y7Ju91duEh8A";
    static constexpr std::string_view kSecret = "NhqPtmdSJYdKjVHjA7PZj4Mge3R5YNiP1e3UZjInClVN65XAbvqqM6A7H5fATj0j";

    explicit BoundTestCredentials(const char* tag = "creds") {
#ifdef _WIN32
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        env_path_ = std::string(tmp) + "hy_test_" + tag + "_" + std::to_string(GetCurrentProcessId()) + ".env";
#else
        env_path_ = std::string("/tmp/hy_test_") + tag + "_" + std::to_string(getpid()) + ".env";
#endif
        {
            std::ofstream f(env_path_, std::ios::binary);
            f << "HENGYUAN_BINANCE_TESTNET_API_KEY=" << kApiKey << "\n"
              << "HENGYUAN_BINANCE_TESTNET_SECRET=" << kSecret << "\n";
        }
#ifdef __linux__
        chmod(env_path_.c_str(), 0600);  // the Linux loader refuses a file other users can read
#endif
        static constexpr std::string_view kAllowedKeys[] = {"HENGYUAN_BINANCE_TESTNET_API_KEY", "HENGYUAN_BINANCE_TESTNET_SECRET"};
        static constexpr hy::EnvAllowlist kAllowlist{kAllowedKeys, 2};
        loader_ = std::make_unique<hy::SecureEnvLoader>();
        const auto r = loader_->load(env_path_.c_str(), kAllowlist);
        if (r.status != hy::EnvLoadStatus::Ok) return;
        const auto binding = hy::EnvironmentBinding::testnet();
        auto [err, creds] = hy::BoundHmacCredentials::load_and_bind_credentials(binding, *loader_);
        if (err != hy::QuerySigningError::Ok) return;
        creds_ = std::move(creds);
    }

    ~BoundTestCredentials() { std::remove(env_path_.c_str()); }

    BoundTestCredentials(const BoundTestCredentials&) = delete;
    BoundTestCredentials& operator=(const BoundTestCredentials&) = delete;

    [[nodiscard]] bool ok() const noexcept { return creds_ != nullptr; }
    [[nodiscard]] hy::BoundHmacCredentials& get() noexcept { return *creds_; }

private:
    std::string env_path_;
    std::unique_ptr<hy::SecureEnvLoader> loader_;
    std::unique_ptr<hy::BoundHmacCredentials> creds_;
};

}  // namespace hy::test_helpers
