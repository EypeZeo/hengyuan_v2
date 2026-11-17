// SPDX-License-Identifier: proprietary
#pragma once

#include <hengyuan/durable_frame_codec.hpp>
#include <hengyuan/durable_log_store.hpp>
#if defined(_WIN32)
#include <hengyuan/windows_native_io.hpp>
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <istream>
#include <optional>
#include <span>
#include <string_view>

namespace hy::audit_archive {

// Wire layout: header, original tip anchor, original store identity, original log bytes.
inline constexpr std::string_view kMagic = "HYAUDAR1";
inline constexpr std::size_t kHeaderSize = 32;
inline constexpr std::uint32_t kProducerVersion = 1;
inline constexpr std::string_view kMigrationSuffix = ".s7.archive";

enum class Kind : std::uint32_t { Segment = 1, S7Migration = 2 };

struct Header {
    Kind kind;
    std::int64_t created_utc_ms;
    std::uint64_t log_bytes;
};

inline std::array<std::byte, kHeaderSize> encode_header(const Header& header) noexcept {
    std::array<std::byte, kHeaderSize> bytes{};
    std::memcpy(bytes.data(), kMagic.data(), kMagic.size());
    auto* cursor = bytes.data() + kMagic.size();
    detail::write_u32_le(cursor, static_cast<std::uint32_t>(header.kind));
    detail::write_u32_le(cursor, kProducerVersion);
    detail::write_u64_le(cursor, static_cast<std::uint64_t>(header.created_utc_ms));
    detail::write_u64_le(cursor, header.log_bytes);
    return bytes;
}

inline std::optional<Header> decode_header(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() < kHeaderSize || std::memcmp(bytes.data(), kMagic.data(), kMagic.size()) != 0) return std::nullopt;
    const auto* cursor = bytes.data() + kMagic.size();
    const auto kind = static_cast<Kind>(detail::read_u32_le(cursor));
    const auto producer = detail::read_u32_le(cursor);
    const auto created_utc_ms = detail::read_i64_le(cursor);
    const auto log_bytes = detail::read_u64_le(cursor);
    if ((kind != Kind::Segment && kind != Kind::S7Migration) || producer != kProducerVersion ||
        created_utc_ms <= 0 || log_bytes == 0 || log_bytes > kMaxDurableLogBytes ||
        log_bytes % kOrderEventFrameSize != 0) {
        return std::nullopt;
    }
    return Header{kind, created_utc_ms, log_bytes};
}

inline bool read_exact(std::istream& input, std::span<std::byte> bytes) {
    return static_cast<bool>(input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())));
}

// Compare archived bytes to a source prefix with bounded scratch space.
inline bool matches_bytes(std::istream& input, std::span<const std::byte> source) {
    std::array<std::byte, 8192> buffer;
    while (!source.empty()) {
        const auto count = std::min(source.size(), buffer.size());
        if (!read_exact(input, std::span(buffer).first(count)) ||
            std::memcmp(buffer.data(), source.data(), count) != 0) {
            return false;
        }
        source = source.subspan(count);
    }
    return true;
}

inline bool at_end(std::istream& input) {
    return input.peek() == std::istream::traits_type::eof() && input.eof() && !input.bad();
}

inline bool sync_directory(const std::filesystem::path& archive) {
    const auto directory = std::filesystem::absolute(archive).parent_path();
#if defined(_WIN32)
    win_native::RawHandle handle;
    return win_native::open_directory(directory.wstring(), handle) == win_native::DirOpenResult::Opened &&
           FlushFileBuffers(handle.get()) != 0;
#else
    const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    int result;
    do {
        result = ::fsync(fd);
    } while (result != 0 && errno == EINTR);
    ::close(fd);
    return result == 0;
#endif
}

}  // namespace hy::audit_archive
