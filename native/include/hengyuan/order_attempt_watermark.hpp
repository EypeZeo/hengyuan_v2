#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

namespace hy {

// Closed generated order attempts advance by (UTC milliseconds, sequence) across the
// lifetime of a store. Recovery restores this bound before accepting new intents.
// A clock rollback or sequence reuse therefore refuses an attempt without I/O.
class OrderAttemptWatermark {
    struct Attempt {
        std::int64_t timestamp;
        std::uint32_t sequence;
    };
    static std::optional<Attempt> parse(std::string_view id) noexcept {
        if (!id.starts_with("HY-")) return std::nullopt;
        id.remove_prefix(3);
        const auto first = id.find('-');
        if (first == std::string_view::npos) return std::nullopt;
        Attempt value{};
        const auto time = std::from_chars(id.data(), id.data() + first, value.timestamp);
        if (time.ec != std::errc{} || time.ptr != id.data() + first || value.timestamp < 0) return std::nullopt;
        id.remove_prefix(first + 1);
        const auto second = id.find('-');
        if (second == std::string_view::npos || id.size() - second - 1 != 4) return std::nullopt;
        const auto sequence = std::from_chars(id.data(), id.data() + second, value.sequence);
        if (sequence.ec != std::errc{} || sequence.ptr != id.data() + second) return std::nullopt;
        std::uint32_t symbol{};
        const auto suffix = std::from_chars(id.data() + second + 1, id.data() + id.size(), symbol, 16);
        if (suffix.ec != std::errc{} || suffix.ptr != id.data() + id.size()) return std::nullopt;
        return value;
    }
    static bool newer(const Attempt& a, const Attempt& b) noexcept {
        return a.timestamp > b.timestamp || (a.timestamp == b.timestamp && a.sequence > b.sequence);
    }
public:
    bool permits(std::string_view id) const noexcept {
        const auto value = parse(id);
        return !value || !latest_ || newer(*value, *latest_);
    }
    void observe(std::string_view id) noexcept {
        const auto value = parse(id);
        if (value && (!latest_ || newer(*value, *latest_))) latest_ = value;
    }
private:
    std::optional<Attempt> latest_{};
};

}  // namespace hy
