// SPDX-License-Identifier: proprietary
// env_parser.hpp — L1 synthetic key=value parser with variable-name allowlist.
//
// Governance: L1 —合成 key=value 解析器, no real secret, no file I/O, no network.
// Real .env loading (file open, path validation, permission check, memory wipe)
// requires L3 authorization per ADR-019 D4 / M1.
//
// Design:
//   - Parses KEY=VALUE lines from an in-memory buffer (not from file path).
//   - Strict variable-name allowlist: only pre-declared keys are accepted.
//   - Unknown keys → rejected (fail-closed, per ADR-019 D4).
//   - Comment lines (#) and empty lines are skipped.
//   - No heap allocation: results written to caller-provided fixed-capacity storage.
//   - Values are not copied out of the source buffer — returned as string_view.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace hy {

static constexpr std::size_t kMaxEnvKeys = 16;
static constexpr std::size_t kMaxKeyLen = 64;

enum class EnvParseStatus : std::uint8_t {
    Ok = 0,
    UnknownKey = 1,
    MalformedLine = 2,
    DuplicateKey = 3,
    KeyTooLong = 4,
    TooManyEntries = 5,
};

struct EnvEntry {
    std::string_view key{};
    std::string_view value{};
};

struct EnvParseResult {
    EnvParseStatus status{EnvParseStatus::Ok};
    std::size_t entry_count{0};
    std::size_t line_number{0};  // 1-based, set on error
    std::array<EnvEntry, kMaxEnvKeys> entries{};

    std::string_view get(std::string_view key) const noexcept {
        for (std::size_t i = 0; i < entry_count; ++i) {
            if (entries[i].key == key) return entries[i].value;
        }
        return {};
    }

    bool has(std::string_view key) const noexcept {
        for (std::size_t i = 0; i < entry_count; ++i) {
            if (entries[i].key == key) return true;
        }
        return false;
    }
};

struct EnvAllowlist {
    const std::string_view* keys{nullptr};
    std::size_t count{0};

    bool contains(std::string_view k) const noexcept {
        for (std::size_t i = 0; i < count; ++i) {
            if (keys[i] == k) return true;
        }
        return false;
    }
};

inline EnvParseResult parse_env_buffer(
    std::string_view buf,
    const EnvAllowlist& allowlist) noexcept {

    EnvParseResult result{};
    std::size_t line_num = 0;
    std::size_t pos = 0;

    while (pos < buf.size()) {
        ++line_num;

        // Find end of line
        auto eol = buf.find('\n', pos);
        std::string_view line;
        if (eol == std::string_view::npos) {
            line = buf.substr(pos);
            pos = buf.size();
        } else {
            line = buf.substr(pos, eol - pos);
            pos = eol + 1;
        }

        // Strip \r
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        // Strip leading/trailing whitespace
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            line.remove_prefix(1);
        }
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
            line.remove_suffix(1);
        }

        // Skip empty lines and comments
        if (line.empty() || line.front() == '#') {
            continue;
        }

        // Find '='
        auto eq = line.find('=');
        if (eq == std::string_view::npos || eq == 0) {
            result.status = EnvParseStatus::MalformedLine;
            result.line_number = line_num;
            return result;
        }

        auto key = line.substr(0, eq);
        auto value = line.substr(eq + 1);

        // Strip trailing whitespace from key
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) {
            key.remove_suffix(1);
        }

        // Strip leading whitespace from value
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
            value.remove_prefix(1);
        }

        // Strip surrounding quotes from value (single or double)
        if (value.size() >= 2) {
            if ((value.front() == '"' && value.back() == '"') ||
                (value.front() == '\'' && value.back() == '\'')) {
                value.remove_prefix(1);
                value.remove_suffix(1);
            }
        }

        if (key.empty()) {
            result.status = EnvParseStatus::MalformedLine;
            result.line_number = line_num;
            return result;
        }

        if (key.size() > kMaxKeyLen) {
            result.status = EnvParseStatus::KeyTooLong;
            result.line_number = line_num;
            return result;
        }

        // Allowlist check (fail-closed per ADR-019 D4)
        if (!allowlist.contains(key)) {
            result.status = EnvParseStatus::UnknownKey;
            result.line_number = line_num;
            return result;
        }

        // Duplicate check
        if (result.has(key)) {
            result.status = EnvParseStatus::DuplicateKey;
            result.line_number = line_num;
            return result;
        }

        // Capacity check
        if (result.entry_count >= kMaxEnvKeys) {
            result.status = EnvParseStatus::TooManyEntries;
            result.line_number = line_num;
            return result;
        }

        result.entries[result.entry_count] = EnvEntry{key, value};
        ++result.entry_count;
    }

    return result;
}

}  // namespace hy
