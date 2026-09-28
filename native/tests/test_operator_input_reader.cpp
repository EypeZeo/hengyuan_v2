// SPDX-License-Identifier: proprietary
#include <hengyuan/operator_input_reader.hpp>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <thread>
#include <utility>

namespace hy {
namespace {

using namespace std::chrono_literals;

struct ScriptedSource {
    std::array<OperatorByteResult, 16> events{};
    std::size_t count{0};
    std::size_t next{0};
    std::chrono::milliseconds last_delay{0};

    void add(char byte) { events[count++] = {OperatorByteStatus::Byte, byte}; }
    void add(OperatorByteStatus status) { events[count++] = {status}; }

    static OperatorByteResult read(void* context, std::chrono::milliseconds timeout) noexcept {
        auto& source = *static_cast<ScriptedSource*>(context);
        if (timeout.count() <= 0 || source.next == source.count) return {OperatorByteStatus::Error};
        if (source.next == source.count - 1) std::this_thread::sleep_for(source.last_delay);
        return source.events[source.next++];
    }
};

// A process-local stdin pipe checks the native timeout/EOF path as well as the
// injected pure-logic tests. Each GTest case runs in its own CTest process.
class StdinPipe {
public:
    StdinPipe() noexcept {
#ifdef _WIN32
        original_ = GetStdHandle(STD_INPUT_HANDLE);
        if (!CreatePipe(&reader_, &writer_, nullptr, 0)) return;
        valid_ = SetStdHandle(STD_INPUT_HANDLE, reader_) != 0;
#else
        original_ = dup(STDIN_FILENO);
        int ends[2]{};
        if (original_ < 0 || pipe(ends) != 0) return;
        reader_ = ends[0];
        writer_ = ends[1];
        valid_ = dup2(reader_, STDIN_FILENO) >= 0;
#endif
    }

    StdinPipe(const StdinPipe&) = delete;
    StdinPipe& operator=(const StdinPipe&) = delete;

    ~StdinPipe() {
#ifdef _WIN32
        if (valid_) SetStdHandle(STD_INPUT_HANDLE, original_);
        if (reader_) CloseHandle(reader_);
        if (writer_) CloseHandle(writer_);
#else
        if (valid_) dup2(original_, STDIN_FILENO);
        if (original_ >= 0) close(original_);
        if (reader_ >= 0) close(reader_);
        if (writer_ >= 0) close(writer_);
#endif
    }

    [[nodiscard]] bool valid() const noexcept { return valid_; }

    bool write_text(const char* text, std::size_t size) noexcept {
#ifdef _WIN32
        DWORD written = 0;
        return WriteFile(writer_, text, static_cast<DWORD>(size), &written, nullptr) != 0 &&
               static_cast<std::size_t>(written) == size;
#else
        return write(writer_, text, size) == static_cast<ssize_t>(size);
#endif
    }

    void close_writer() noexcept {
#ifdef _WIN32
        CloseHandle(writer_);
        writer_ = nullptr;
#else
        close(writer_);
        writer_ = -1;
#endif
    }

private:
#ifdef _WIN32
    HANDLE original_{nullptr};
    HANDLE reader_{nullptr};
    HANDLE writer_{nullptr};
#else
    int original_{-1};
    int reader_{-1};
    int writer_{-1};
#endif
    bool valid_{false};
};

TEST(OperatorInputReader, ReadsOneExactLineAndCannotReadAgain) {
    ScriptedSource source;
    for (char byte : "CONFIRM\n") {
        if (byte != '\0') source.add(byte);
    }
    OperatorInputReader reader(&ScriptedSource::read, &source);
    const auto first = reader.read_once(100ms);
    EXPECT_EQ(first.status, OperatorInputStatus::Line);
    EXPECT_EQ(first.line(), "CONFIRM");
    EXPECT_EQ(reader.read_once(100ms).status, OperatorInputStatus::AlreadyRead);
}

TEST(OperatorInputReader, TimeoutEofAndErrorAreDistinctFailures) {
    for (const auto& [byte_status, expected] : {
             std::pair{OperatorByteStatus::Timeout, OperatorInputStatus::Timeout},
             std::pair{OperatorByteStatus::EndOfInput, OperatorInputStatus::EndOfInput},
             std::pair{OperatorByteStatus::Error, OperatorInputStatus::Error}}) {
        ScriptedSource source;
        source.add('C');  // incomplete confirmation must not be accepted
        source.add(byte_status);
        OperatorInputReader reader(&ScriptedSource::read, &source);
        EXPECT_EQ(reader.read_once(100ms).status, expected);
    }
    ScriptedSource source;
    OperatorInputReader reader(&ScriptedSource::read, &source);
    EXPECT_EQ(reader.read_once(0ms).status, OperatorInputStatus::Timeout);
    EXPECT_EQ(source.next, 0u);
}

TEST(OperatorInputReader, LateNewlineDoesNotCompleteTimedOutInput) {
    ScriptedSource source;
    for (char byte : "CONFIRM\n") {
        if (byte != '\0') source.add(byte);
    }
    source.last_delay = 30ms;
    OperatorInputReader reader(&ScriptedSource::read, &source);
    EXPECT_EQ(reader.read_once(5ms).status, OperatorInputStatus::Timeout);
}

TEST(OperatorInputReader, OverlongLineAndMissingSourceFailClosed) {
    ScriptedSource source;
    for (char byte : "CONFIRMX\n") {
        if (byte != '\0') source.add(byte);
    }
    OperatorInputReader reader(&ScriptedSource::read, &source);
    EXPECT_EQ(reader.read_once(100ms).status, OperatorInputStatus::TooLong);
    EXPECT_EQ(source.next, source.count);  // the suffix cannot become a later prompt's input

    OperatorInputReader missing(nullptr, nullptr);
    EXPECT_EQ(missing.read_once(100ms).status, OperatorInputStatus::Error);
}

TEST(OperatorInputReader, CarriageReturnEndsWindowsConsoleLine) {
    ScriptedSource source;
    source.add('N');
    source.add('O');
    source.add('\r');
    OperatorInputReader reader(&ScriptedSource::read, &source);
    const auto result = reader.read_once(100ms);
    EXPECT_EQ(result.status, OperatorInputStatus::Line);
    EXPECT_EQ(result.line(), "NO");
}

TEST(OperatorInputReader, NativeStdinPipeReadsLineThenReportsEof) {
    StdinPipe pipe;
    ASSERT_TRUE(pipe.valid());
    ASSERT_TRUE(pipe.write_text("CONFIRM\n", 8));
    pipe.close_writer();
    OperatorInputReader reader(&read_operator_stdin_byte, nullptr);
    const auto result = reader.read_once(100ms);
    EXPECT_EQ(result.status, OperatorInputStatus::Line);
    EXPECT_EQ(result.line(), "CONFIRM");
    EXPECT_EQ(read_operator_stdin_byte(nullptr, 100ms).status, OperatorByteStatus::EndOfInput);
}

TEST(OperatorInputReader, NativeStdinPipeTimesOutWithoutInput) {
    StdinPipe pipe;
    ASSERT_TRUE(pipe.valid());
    OperatorInputReader reader(&read_operator_stdin_byte, nullptr);
    EXPECT_EQ(reader.read_once(20ms).status, OperatorInputStatus::Timeout);
}

}  // namespace
}  // namespace hy
