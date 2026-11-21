// SPDX-License-Identifier: proprietary
#pragma once

#include <hengyuan/spot_owner_key_store.hpp>

#include <array>
#include <charconv>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace hy::scope_files {

inline std::string hex_id(const std::array<std::byte, 16>& bytes) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string name;
    name.reserve(2 * bytes.size());
    for (const auto byte : bytes) {
        const auto value = std::to_integer<unsigned>(byte);
        name.push_back(digits[value >> 4]);
        name.push_back(digits[value & 15]);
    }
    return name;
}

template <typename Number>
bool parse_number(std::string_view token, Number& number) noexcept {
    if (token.empty()) return false;
    const auto end = token.data() + token.size();
    const auto result = std::from_chars(token.data(), end, number);
    return result.ec == std::errc{} && result.ptr == end && number > 0;
}

inline ExportStatus publish_text(const std::filesystem::path& path, std::string_view text) {
    return spot_owner_key_store::publish_file(path.string(), std::as_bytes(std::span(text)));
}

inline std::optional<std::filesystem::path> publish_request(
    const std::filesystem::path& directory, std::string_view operation, std::span<const std::byte> payload) {
    std::error_code ec;
    if (!std::filesystem::is_directory(std::filesystem::symlink_status(directory, ec)) || ec) return std::nullopt;
    std::array<std::byte, 16> nonce{};
    if (!detail::fill_random_bytes(nonce)) return std::nullopt;
    const auto path = directory / (std::string(operation) + "-" + hex_id(nonce) + ".req");
    if (spot_owner_key_store::publish_file(path.string(), payload) != ExportStatus::Ok) return std::nullopt;
    return path;
}

struct Request {
    std::filesystem::path claimed_file;
    std::filesystem::path result_file;
};

// One process claims requests before reading them. Only the current activation
// directory is polled, so a restart never retries a previous order request.
inline std::optional<Request> claim_request(const std::filesystem::path& directory, std::string_view operation) {
    const auto prefix = std::string(operation) + "-";
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        const auto name = entry.path().filename().string();
        const std::string_view view = name;
        if (!view.starts_with(prefix) || !view.ends_with(".req") || view.size() != prefix.size() + 32 + 4) continue;
        const auto nonce = view.substr(prefix.size(), 32);
        if (nonce.find_first_not_of("0123456789abcdef") != std::string_view::npos) continue;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(entry.symlink_status(ec)) || ec) continue;
        auto claimed = entry.path();
        claimed.replace_extension(".claimed");
        if (std::filesystem::exists(std::filesystem::symlink_status(claimed, ec))) continue;
        std::filesystem::rename(entry.path(), claimed, ec);
        if (ec) continue;
        auto result = claimed;
        result.replace_extension(".result");
        return Request{std::move(claimed), std::move(result)};
    }
    return std::nullopt;
}

}  // namespace hy::scope_files
