// SPDX-License-Identifier: proprietary
// operator_resolution_terminal.hpp -- the offline operator tool's console and evidence directory (D5 slice R3b-2).
//
// Governance: offline tool only. The two real implementations of the ports the flow reaches the operator and the disk through
// (operator_resolution_ports.hpp): ConsolePort over a C stream and the native stdin adapter, EvidenceStorePort over a directory. Neither
// decides anything -- the flow decides what is shown, what is asked and what is stored; these only move bytes, and refuse to do it
// carelessly:
//
//   * StdConsole::write_line() writes printable ASCII only. Every line the flow composes already is (the account summary is composed by
//     summarize_account() from parsed fields, the rest are the flow's own words and validated identifiers), so this is the last hop's
//     defence in depth: a byte that is not printable -- an escape sequence that would rewrite what the operator sees on his terminal --
//     comes out as '?'. On a Windows console, the confirmation echoes characters and handles Backspace using the console buffers.
//     Injected sources and redirected streams use a fresh bounded OperatorLineReader for each attempt.
//   * DirectoryEvidenceStore::store() creates `<directory>/<stem>.json` EXCLUSIVELY (an existing file is never touched, let alone
//     overwritten), writes it, flushes it to disk, and -- where the platform has such a thing -- flushes the directory too, before it says
//     true. The flow writes the frame only after that. A failure at any step leaves no file behind. The directory is created if it does not
//     exist (one level, owner-only where the platform has that notion), and anything but a directory in its place is a failure.
//     Windows has no directory flush through the calls used here: FlushFileBuffers on the file is all that is done there.
//     `stem` is [A-Za-z0-9._-], 1 to 128 characters: a path separator, a drive colon or ".." cannot be in it.

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <hengyuan/durable_audit_export.hpp>  // ExclusiveOutputFile
#include <hengyuan/operator_input_reader.hpp>
#include <hengyuan/operator_resolution_confirmation.hpp>
#include <hengyuan/operator_resolution_ports.hpp>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace hy::operator_resolution {

// --- the console -------------------------------------------------------------------------------------------------------------------

#ifdef _WIN32
namespace detail {

inline bool write_confirmation_console(HANDLE output, std::wstring_view text) noexcept {
    const auto length = static_cast<DWORD>(text.size());
    DWORD written = 0;
    return WriteConsoleW(output, text.data(), length, &written, nullptr) != 0 && written == length;
}

inline bool erase_confirmation_console_tail(HANDLE output) noexcept {
    CONSOLE_SCREEN_BUFFER_INFO screen{};
    if (!GetConsoleScreenBufferInfo(output, &screen)) return false;
    COORD previous = screen.dwCursorPosition;
    if (previous.X > 0) {
        --previous.X;
    } else {
        if (previous.Y == 0) return false;
        --previous.Y;
        previous.X = static_cast<SHORT>(screen.dwSize.X - 1);
    }
    DWORD written = 0;
    return WriteConsoleOutputCharacterW(output, L" ", 1, previous, &written) != 0 && written == 1 &&
           SetConsoleCursorPosition(output, previous) != 0;
}

inline OperatorLineResult read_windows_confirmation_keys(HANDLE input, HANDLE output,
                                                         std::chrono::steady_clock::time_point deadline) noexcept {
    OperatorLineResult result{OperatorInputStatus::Line};
    std::array<wchar_t, kConfirmationLineMaxBytes> echo{};
    std::uint64_t overflow = 0;
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return OperatorLineResult{OperatorInputStatus::Timeout};
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
        const DWORD wait = WaitForSingleObject(input, static_cast<DWORD>(remaining.count()));
        if (wait == WAIT_TIMEOUT) return OperatorLineResult{OperatorInputStatus::Timeout};
        if (wait != WAIT_OBJECT_0) return OperatorLineResult{OperatorInputStatus::Error};

        INPUT_RECORD record{};
        DWORD read = 0;
        if (!ReadConsoleInputW(input, &record, 1, &read) || read != 1) return OperatorLineResult{OperatorInputStatus::Error};
        if (std::chrono::steady_clock::now() >= deadline) return OperatorLineResult{OperatorInputStatus::Timeout};
        if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown) continue;

        const KEY_EVENT_RECORD& key = record.Event.KeyEvent;
        const wchar_t character = key.uChar.UnicodeChar;
        if (character == L'\r' || character == L'\n' || key.wVirtualKeyCode == VK_RETURN) {
            return overflow == 0 ? result : OperatorLineResult{OperatorInputStatus::TooLong};
        }
        const auto repeats = static_cast<std::size_t>(key.wRepeatCount);
        if (character == L'\b' || key.wVirtualKeyCode == VK_BACK) {
            const auto hidden = static_cast<std::size_t>(std::min<std::uint64_t>(overflow, repeats));
            overflow -= hidden;
            const auto visible = std::min(result.size, repeats - hidden);
            for (std::size_t i = 0; i < visible; ++i) {
                if (!erase_confirmation_console_tail(output)) return OperatorLineResult{OperatorInputStatus::Error};
            }
            result.size -= visible;
            continue;
        }
        if (character == L'\0') continue;  // a modifier or function key produced no input character

        // Keep non-ASCII and control input invalid even though its console representation is sanitized.
        const bool printable = character >= L' ' && character <= L'~';
        const auto visible = std::min(repeats, result.bytes.size() - result.size);
        std::fill_n(result.bytes.data() + result.size, visible, printable ? static_cast<char>(character) : '\0');
        result.size += visible;
        overflow += repeats - visible;
        if (visible != 0) {
            std::fill_n(echo.data(), visible, printable ? character : L'?');
            if (!write_confirmation_console(output, {echo.data(), visible})) return OperatorLineResult{OperatorInputStatus::Error};
        }
    }
}

inline OperatorLineResult read_windows_confirmation_line(HANDLE input, HANDLE output,
                                                         std::chrono::milliseconds timeout) noexcept {
    if (timeout.count() <= 0) return OperatorLineResult{OperatorInputStatus::Timeout};
    if (timeout > std::chrono::minutes{5}) return OperatorLineResult{OperatorInputStatus::Error};

    DWORD input_mode = 0;
    DWORD saved_mode = 0;
    if (!GetConsoleMode(input, &input_mode) || !GetConsoleMode(output, &saved_mode)) {
        return OperatorLineResult{OperatorInputStatus::Error};
    }
    // With immediate wrapping, moving back one screen cell also works at the start of a row.
    const DWORD editing_mode = (saved_mode | ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT)
                               & ~static_cast<DWORD>(DISABLE_NEWLINE_AUTO_RETURN);
    if (editing_mode != saved_mode && !SetConsoleMode(output, editing_mode)) return OperatorLineResult{OperatorInputStatus::Error};

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    OperatorLineResult result{OperatorInputStatus::Error};
    if (write_confirmation_console(output, L"Confirm> ")) {
        result = read_windows_confirmation_keys(input, output, deadline);
        if (!write_confirmation_console(output, L"\r\n")) result = OperatorLineResult{OperatorInputStatus::Error};
    }
    if (editing_mode != saved_mode && !SetConsoleMode(output, saved_mode)) return OperatorLineResult{OperatorInputStatus::Error};
    return result;
}

}  // namespace detail
#endif

class StdConsole final : public ConsolePort {
public:
    // `out`: where the lines go (stdout for the dialogue, stderr for the tool's own diagnostics). `read`/`read_user_data`: the byte source of
    // read_line().
    explicit StdConsole(std::FILE* out, OperatorTimedRead read = &read_operator_stdin_byte, void* read_user_data = nullptr) noexcept
        : out_(out), read_(read), read_user_data_(read_user_data) {}

    StdConsole(const StdConsole&) = delete;
    StdConsole& operator=(const StdConsole&) = delete;

    void write_line(std::string_view line) override {
        if (out_ == nullptr) return;
        for (const char c : line) std::fputc(is_printable_ascii(c) ? c : '?', out_);
        std::fputc('\n', out_);
        std::fflush(out_);
    }

    OperatorLineResult read_line(std::chrono::milliseconds timeout) override {
#ifdef _WIN32
        if (out_ == stdout && read_ == &read_operator_stdin_byte) {
            const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
            const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
            DWORD mode = 0;
            if (GetConsoleMode(input, &mode) && GetConsoleMode(output, &mode)) {
                return detail::read_windows_confirmation_line(input, output, timeout);
            }
        }
#endif
        OperatorLineReader reader(read_, read_user_data_);
        return reader.read_once(timeout);
    }

    static constexpr bool is_printable_ascii(char c) noexcept {
        const unsigned char u = static_cast<unsigned char>(c);
        return u >= 0x20 && u <= 0x7e;
    }

private:
    std::FILE* out_;
    OperatorTimedRead read_;
    void* read_user_data_;
};

// --- the evidence directory --------------------------------------------------------------------------------------------------------

inline constexpr std::string_view kEvidenceFileSuffix = ".json";
inline constexpr std::size_t kEvidenceStemMaxChars = 128;
// <audit log>.operator-evidence (design 6 step 8)
inline constexpr std::string_view kEvidenceDirectorySuffix = ".operator-evidence";

constexpr bool is_evidence_stem(std::string_view stem) noexcept {
    if (stem.empty() || stem.size() > kEvidenceStemMaxChars) return false;
    for (const char c : stem) {
        if (!(detail::is_ascii_alnum(c) || c == '.' || c == '_' || c == '-')) return false;
    }
    return true;
}

class DirectoryEvidenceStore final : public EvidenceStorePort {
public:
    explicit DirectoryEvidenceStore(std::string directory) : directory_(std::move(directory)) {}

    DirectoryEvidenceStore(const DirectoryEvidenceStore&) = delete;
    DirectoryEvidenceStore& operator=(const DirectoryEvidenceStore&) = delete;

    const std::string& directory() const noexcept { return directory_; }

    // Where `file_stem` is stored; empty for a stem that is not acceptable (see is_evidence_stem()) or without a directory.
    std::string path_for(std::string_view file_stem) const {
        if (directory_.empty() || !is_evidence_stem(file_stem)) return {};
        std::string name(file_stem);
        name += kEvidenceFileSuffix;
        return (std::filesystem::path(directory_) / name).string();
    }

    bool store(std::string_view file_stem, std::string_view contents) override {
        try {
            const std::string path = path_for(file_stem);
            if (path.empty() || !ensure_directory()) return false;
            ExclusiveOutputFile file;  // removes what it created unless commit() succeeded
            if (file.create(path) != ExclusiveOutputFile::OpenStatus::Ok) return false;  // Exists: never touched
            if (!file.write(contents) || !file.commit()) return false;
            if (!sync_directory()) {
                std::error_code ignored;
                std::filesystem::remove(path, ignored);  // the entry is not known to be durable: do not leave it looking like evidence
                return false;
            }
            return true;
        } catch (...) {
            return false;
        }
    }

private:
    bool ensure_directory() const {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::file_status status = fs::status(directory_, ec);
        if (!ec && fs::is_directory(status)) return true;
        if (!ec && fs::exists(status)) return false;  // a file (or something else) is in the way
        ec.clear();
        fs::create_directory(directory_, ec);  // one level: the parent is the audit log's directory and exists
        if (ec) return false;
        std::error_code ignored;
        fs::permissions(directory_, fs::perms::owner_all, fs::perm_options::replace, ignored);  // owner-only where that means anything
        return fs::is_directory(directory_, ignored);
    }

    bool sync_directory() const noexcept {
#ifdef _WIN32
        return true;  // see the header comment: nothing more can be done from here
#else
        const int fd = ::open(directory_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) return false;
        const bool ok = ::fsync(fd) == 0;
        ::close(fd);
        return ok;
#endif
    }

    std::string directory_;
};

}  // namespace hy::operator_resolution
