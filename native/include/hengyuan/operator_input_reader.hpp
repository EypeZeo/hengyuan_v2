// SPDX-License-Identifier: proprietary
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <poll.h>
#include <unistd.h>
#endif

namespace hy {

enum class OperatorByteStatus : std::uint8_t { Byte, Timeout, EndOfInput, Error };

struct OperatorByteResult {
    OperatorByteStatus status{OperatorByteStatus::Error};
    char byte{0};

    constexpr OperatorByteResult() noexcept = default;
    constexpr OperatorByteResult(OperatorByteStatus status_value, char byte_value = 0) noexcept
        : status(status_value), byte(byte_value) {}
};

using OperatorTimedRead = OperatorByteResult (*)(void*, std::chrono::milliseconds) noexcept;

enum class OperatorInputStatus : std::uint8_t {
    Line,
    Timeout,
    EndOfInput,
    Error,
    TooLong,
    AlreadyRead,
};

struct OperatorInputResult {
    OperatorInputStatus status{OperatorInputStatus::Error};
    std::array<char, 7> bytes{};  // exact ASCII "CONFIRM" is the only accepted line
    std::size_t size{0};

    constexpr OperatorInputResult() noexcept = default;
    constexpr OperatorInputResult(OperatorInputStatus status_value) noexcept
        : status(status_value) {}

    [[nodiscard]] std::string_view line() const noexcept {
        return {bytes.data(), size <= bytes.size() ? size : bytes.size()};
    }
};

// One instance owns one read attempt. No worker thread is left behind on
// timeout, EOF, or error. The source function must honor its timeout; the
// native stdin adapter below does so for console, pipe, and POSIX fd input.
class OperatorInputReader {
public:
    OperatorInputReader(OperatorTimedRead read, void* user_data) noexcept
        : read_(read), user_data_(user_data) {}

    OperatorInputReader(const OperatorInputReader&) = delete;
    OperatorInputReader& operator=(const OperatorInputReader&) = delete;

    [[nodiscard]] OperatorInputResult read_once(std::chrono::milliseconds timeout) noexcept {
        if (used_) return {OperatorInputStatus::AlreadyRead};
        used_ = true;
        if (!read_) return {OperatorInputStatus::Error};
        if (timeout.count() <= 0) return {OperatorInputStatus::Timeout};
        // Bound both the API's duration arithmetic and a human confirmation's
        // maximum wait. A longer request is an invalid control-plane input.
        if (timeout > std::chrono::minutes{5}) return {OperatorInputStatus::Error};

        const auto deadline = std::chrono::steady_clock::now() + timeout;
        OperatorInputResult result{};
        bool overlong = false;
        for (;;) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return {OperatorInputStatus::Timeout};
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            if (remaining.count() == 0) remaining = std::chrono::milliseconds{1};
            const OperatorByteResult next = read_(user_data_, remaining);
            if (std::chrono::steady_clock::now() >= deadline) {
                return {OperatorInputStatus::Timeout};
            }
            switch (next.status) {
                case OperatorByteStatus::Timeout: return {OperatorInputStatus::Timeout};
                case OperatorByteStatus::EndOfInput: return {OperatorInputStatus::EndOfInput};
                case OperatorByteStatus::Error: return {OperatorInputStatus::Error};
                case OperatorByteStatus::Byte: break;
            }
            if (next.byte == '\n' || next.byte == '\r') {
                result.status = overlong ? OperatorInputStatus::TooLong : OperatorInputStatus::Line;
                return result;
            }
            // Drain the rest of an overlong line so that a later challenge
            // cannot accidentally consume its suffix as a fresh CONFIRM.
            if (result.size == result.bytes.size()) {
                overlong = true;
            } else {
                result.bytes[result.size++] = next.byte;
            }
        }
    }

private:
    OperatorTimedRead read_{nullptr};
    void* user_data_{nullptr};
    bool used_{false};
};

// This is the sole native stdin adapter for a future owner-thread integration.
// It deliberately never spawns a thread or calls std::getline (which cannot be
// cancelled on a timeout). Do not call it concurrently from multiple readers.
inline OperatorByteResult read_operator_stdin_byte(void*, std::chrono::milliseconds timeout) noexcept {
    if (timeout.count() <= 0) return {OperatorByteStatus::Timeout};
#ifdef _WIN32
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if (input == nullptr || input == INVALID_HANDLE_VALUE) return {OperatorByteStatus::Error};
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    DWORD console_mode = 0;
    const bool is_console = GetConsoleMode(input, &console_mode) != 0;
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return {OperatorByteStatus::Timeout};
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        if (remaining.count() == 0) remaining = std::chrono::milliseconds{1};
        if (is_console) {
            const DWORD wait = WaitForSingleObject(input, static_cast<DWORD>(remaining.count()));
            if (wait == WAIT_TIMEOUT) return {OperatorByteStatus::Timeout};
            if (wait != WAIT_OBJECT_0) return {OperatorByteStatus::Error};
            INPUT_RECORD record{};
            DWORD read_count = 0;
            if (!ReadConsoleInputA(input, &record, 1, &read_count) || read_count != 1) {
                return {OperatorByteStatus::Error};
            }
            if (record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown) {
                const char byte = record.Event.KeyEvent.uChar.AsciiChar;
                if (byte != 0) return {OperatorByteStatus::Byte, byte};
            }
            continue;
        }
        if (GetFileType(input) == FILE_TYPE_PIPE) {
            DWORD available = 0;
            if (!PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr)) {
                return GetLastError() == ERROR_BROKEN_PIPE ? OperatorByteResult{OperatorByteStatus::EndOfInput}
                                                       : OperatorByteResult{OperatorByteStatus::Error};
            }
            if (available == 0) {
                Sleep(static_cast<DWORD>(remaining.count() < 10 ? remaining.count() : 10));
                continue;
            }
        } else if (GetFileType(input) != FILE_TYPE_DISK) {
            return {OperatorByteStatus::Error};
        }
        char byte = 0;
        DWORD read_count = 0;
        if (!ReadFile(input, &byte, 1, &read_count, nullptr)) {
            return GetLastError() == ERROR_BROKEN_PIPE ? OperatorByteResult{OperatorByteStatus::EndOfInput}
                                                   : OperatorByteResult{OperatorByteStatus::Error};
        }
        if (read_count == 0) return {OperatorByteStatus::EndOfInput};
        return {OperatorByteStatus::Byte, byte};
    }
#else
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return {OperatorByteStatus::Timeout};
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        if (remaining.count() == 0) remaining = std::chrono::milliseconds{1};
        pollfd descriptor{STDIN_FILENO, static_cast<short>(POLLIN | POLLHUP), 0};
        const int ready = poll(&descriptor, 1, static_cast<int>(remaining.count()));
        if (ready == 0) return {OperatorByteStatus::Timeout};
        if (ready < 0) {
            if (errno == EINTR) continue;
            return {OperatorByteStatus::Error};
        }
        if ((descriptor.revents & (POLLERR | POLLNVAL)) != 0) return {OperatorByteStatus::Error};
        char byte = 0;
        const auto read_count = read(STDIN_FILENO, &byte, 1);
        if (read_count == 0) return {OperatorByteStatus::EndOfInput};
        if (read_count == 1) return {OperatorByteStatus::Byte, byte};
        if (errno != EINTR) return {OperatorByteStatus::Error};
    }
#endif
}

}  // namespace hy
