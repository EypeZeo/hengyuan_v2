// SPDX-License-Identifier: proprietary
#pragma once

#include <hengyuan/owner_reset_approval.hpp>
#include <hengyuan/spot_owner_key_store.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hy {

// Owned by the private-stream pipeline's caller. The control directory is
// writable by the local operator, who publishes a complete signature by rename.
class OwnerResetRequestFiles {
public:
    OwnerResetRequestFiles(OwnerResetAuthority& authority, std::filesystem::path directory)
        : authority_(authority), directory_(std::move(directory)) {}

    OwnerResetRequestFiles(const OwnerResetRequestFiles&) = delete;
    OwnerResetRequestFiles& operator=(const OwnerResetRequestFiles&) = delete;

    [[nodiscard]] std::optional<OperatorResetApproval> pending_approval() const noexcept {
        return authority_.pending_approval();
    }
    [[nodiscard]] const std::string& challenge_path() const noexcept { return challenge_path_; }
    [[nodiscard]] const std::string& signature_path() const noexcept { return signature_path_; }

    [[nodiscard]] ExportStatus publish(const OperatorResetTarget& target, std::int64_t server_upper_ms) {
        const auto bytes = authority_.prepare(target, server_upper_ms);
        if (!bytes) return ExportStatus::OutputError;

        std::string name = "owner-reset-";
        constexpr std::string_view digits = "0123456789abcdef";
        for (const auto byte : authority_.activation()) {
            name.push_back(digits[byte >> 4]);
            name.push_back(digits[byte & 15]);
        }
        auto challenge = (directory_ / (name + ".grant")).string();
        auto signature = (directory_ / (name + ".sig")).string();
        const auto status = spot_owner_key_store::publish_file(challenge, *bytes);
        if (status == ExportStatus::Ok) {
            challenge_path_ = std::move(challenge);
            signature_path_ = std::move(signature);
        }
        return status;
    }

    template <typename Apply>
    [[nodiscard]] std::optional<OperatorResetResult> apply_if_ready(Apply&& apply) {
        if (consumed_ || signature_path_.empty() || !spot_owner_key_store::is_regular_file(signature_path_)) {
            return std::nullopt;
        }
        consumed_ = true;
        std::vector<std::byte> signature;
        if (read_file_bounded(signature_path_, signature, owner_signature::kMaxSignatureBytes) != ReadFileStatus::Ok ||
            signature.empty()) return OperatorResetResult::InvalidSignature;
        return apply(spot_owner_key_store::unsigned_bytes(signature));
    }

    [[nodiscard]] bool renew() noexcept {
        if (!authority_.renew_request()) return false;
        challenge_path_.clear();
        signature_path_.clear();
        consumed_ = false;
        return true;
    }

private:
    OwnerResetAuthority& authority_;
    std::filesystem::path directory_;
    std::string challenge_path_;
    std::string signature_path_;
    bool consumed_{false};
};

}  // namespace hy
