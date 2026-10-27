// SPDX-License-Identifier: proprietary
// operator_resolution_confirmation.hpp -- what the operator has to type, and the reader that takes it (D5 slice R3a).
//
// Governance: L1 (no allocation, no exceptions, no I/O of its own: the byte source is injected as an OperatorTimedRead, exactly
// as operator_input_reader.hpp does it). docs/OPERATOR_RESOLUTION_DESIGN.md 6 step 6: before the offline tool may write an
// OrderOperatorResolved frame the operator is shown all of the evidence and must type a phrase that binds the client order id,
// the conclusion, the environment name and the first eight characters of the evidence digest. Two things the design left to the
// implementation are added here because the log cannot vouch for them:
//   * the SYMBOL: the audit log records only a symbol_id, the name comes from the control-plane log, and nothing binds the two
//     logs to each other (blueprint L-50). Typing the name makes the operator look at what the tool is about to ask the exchange.
//   * the ACCOUNT UID: no store records which exchange environment and account an order was sent to (L-48), and fetch_account()
//     discards the uid, so the tool cannot check it either. The operator states it; the tool can only make him say it.
// A phrase that has to be TYPED, not clicked, and that contains a digest only the evidence summary shows, cannot be confirmed
// without reading that summary.
//
// Why a line reader of its own: operator_input_reader.hpp's OperatorInputResult holds seven bytes, because the one phrase it
// accepts is "CONFIRM" for an order authorization. Its byte source (OperatorTimedRead) and the native stdin adapter are reused;
// the reading loop is copied with a larger buffer rather than templated, so the live order-confirmation path is not touched.

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <hengyuan/account_truth.hpp>  // kSymbolNameLen
#include <hengyuan/operator_confirmation_gate.hpp>  // OperatorEnvironment
#include <hengyuan/operator_input_reader.hpp>       // OperatorTimedRead, OperatorInputStatus
#include <hengyuan/order_lifecycle.hpp>             // kClientOrderIdLen

namespace hy::operator_resolution {

inline constexpr std::size_t kConfirmationLineMaxBytes = 192;
inline constexpr std::size_t kEvidenceDigestHexChars = 64;  // a SHA-256 in lower-case hex
inline constexpr std::size_t kDigestPrefixChars = 8;        // how much of it the operator types
inline constexpr std::string_view kConfirmationKeyword = "ABSENT";

// --- grammars ----------------------------------------------------------------------------------------------------------------

namespace detail {

constexpr bool is_ascii_alnum(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

}  // namespace detail

// What the exchange accepts as a client order id (letters, digits and . : / _ -, at most 36), which is also what makes the id
// safe to put into a phrase (no space) and a query (the transport still encodes it).
constexpr bool is_exchange_client_order_id(std::string_view id) noexcept {
    if (id.empty() || id.size() > kClientOrderIdLen) return false;
    for (const char c : id) {
        if (!(detail::is_ascii_alnum(c) || c == '.' || c == ':' || c == '/' || c == '_' || c == '-')) return false;
    }
    return true;
}

// The shape ControlPlaneLogSink::recovered_symbol_name() accepts: 1 to 19 upper-case letters and digits.
constexpr bool is_exchange_symbol(std::string_view s) noexcept {
    if (s.empty() || s.size() >= kSymbolNameLen) return false;
    for (const char c : s) {
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
    }
    return true;
}

// The operator's tag in the frame's detail_msg ("op=<label>;ev=<32 hex>" must fit its 63 characters): 1 to 24 characters.
inline constexpr std::size_t kOperatorLabelMaxChars = 24;

constexpr bool is_operator_label(std::string_view s) noexcept {
    if (s.empty() || s.size() > kOperatorLabelMaxChars) return false;
    for (const char c : s) {
        if (!(detail::is_ascii_alnum(c) || c == '.' || c == '_' || c == '-')) return false;
    }
    return true;
}

constexpr bool is_lower_hex(std::string_view s, std::size_t chars) noexcept {
    if (s.size() != chars) return false;
    for (const char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

constexpr std::string_view environment_name(OperatorEnvironment e) noexcept {
    switch (e) {
        case OperatorEnvironment::Testnet: return "TESTNET";
        case OperatorEnvironment::Production: return "PRODUCTION";
    }
    return {};  // an out-of-range value has no name
}

// --- the phrase ---------------------------------------------------------------------------------------------------------------

struct ConfirmationChallenge {
    std::string_view client_order_id;
    std::string_view symbol;
    OperatorEnvironment environment{static_cast<OperatorEnvironment>(255)};  // invalid until set
    std::uint64_t account_uid{0};                                            // invalid until set
    std::string_view evidence_digest_hex;  // 64 lower-case hex characters: the SHA-256 of the evidence the operator was shown
};

// "ABSENT <client order id> <symbol> <ENVIRONMENT> <account uid> <first 8 digest characters>", for example
//   ABSENT HY-1790000000000-7-0A1B BTCUSDT TESTNET 123456789 3f9a0c1e
struct ConfirmationPhrase {
    bool valid{false};  // false: nothing may be confirmed against this phrase
    std::array<char, kConfirmationLineMaxBytes> text{};
    std::size_t size{0};

    constexpr std::string_view view() const noexcept { return valid ? std::string_view(text.data(), size) : std::string_view{}; }
};

namespace detail {

struct PhraseWriter {
    std::array<char, kConfirmationLineMaxBytes>& buffer;
    std::size_t size{0};
    bool ok{true};

    constexpr void put(std::string_view s) noexcept {
        if (!ok || size + s.size() > buffer.size()) {
            ok = false;
            return;
        }
        for (const char c : s) buffer[size++] = c;
    }
    constexpr void put_decimal(std::uint64_t v) noexcept {
        std::array<char, 20> reversed{};
        std::size_t n = 0;
        do {
            reversed[n++] = static_cast<char>('0' + static_cast<int>(v % 10));
            v /= 10;
        } while (v != 0);
        while (n != 0) put(std::string_view(&reversed[--n], 1));
    }
};

}  // namespace detail

// The longest phrase a VALID challenge can make -- the keyword, a full-length client order id, a 19-character symbol, "PRODUCTION",
// a 20-digit account uid, the digest prefix and the five separating spaces -- fits the line buffer. So running out of room is not
// a case the writer below has to survive for any input it accepts; its own bound check is defence in depth.
inline constexpr std::size_t kLongestPhraseBytes = kConfirmationKeyword.size() + kClientOrderIdLen + (kSymbolNameLen - 1) +
                                                   std::string_view("PRODUCTION").size() + 20 + kDigestPrefixChars + 5;
static_assert(kLongestPhraseBytes <= kConfirmationLineMaxBytes, "a valid challenge's phrase must fit the confirmation line buffer");

// Invalid (valid == false, empty view) if any field is outside its grammar or the environment or the uid is unset. (Its buffer
// overflowing is not a case: see kLongestPhraseBytes.)
constexpr ConfirmationPhrase make_confirmation_phrase(const ConfirmationChallenge& c) noexcept {
    ConfirmationPhrase p{};
    const std::string_view env = environment_name(c.environment);
    if (env.empty() || c.account_uid == 0 || !is_exchange_client_order_id(c.client_order_id) || !is_exchange_symbol(c.symbol) ||
        !is_lower_hex(c.evidence_digest_hex, kEvidenceDigestHexChars)) {
        return p;
    }
    detail::PhraseWriter w{p.text};
    w.put(kConfirmationKeyword);
    w.put(" ");
    w.put(c.client_order_id);
    w.put(" ");
    w.put(c.symbol);
    w.put(" ");
    w.put(env);
    w.put(" ");
    w.put_decimal(c.account_uid);
    w.put(" ");
    w.put(c.evidence_digest_hex.substr(0, kDigestPrefixChars));
    if (!w.ok) return ConfirmationPhrase{};
    p.size = w.size;
    p.valid = true;
    return p;
}

// --- the line the operator typed ---------------------------------------------------------------------------------------------

struct OperatorLineResult {
    OperatorInputStatus status{OperatorInputStatus::Error};  // the zero value is not a line
    std::array<char, kConfirmationLineMaxBytes> bytes{};
    std::size_t size{0};

    constexpr OperatorLineResult() noexcept = default;
    constexpr explicit OperatorLineResult(OperatorInputStatus status_value) noexcept : status(status_value) {}

    [[nodiscard]] std::string_view line() const noexcept {
        return {bytes.data(), size <= bytes.size() ? size : bytes.size()};
    }
};

// One instance owns one read attempt (a second read_once() is AlreadyRead). Both '\n' and '\r' end the line, so a console that
// sends CR LF yields the line at the CR; the LF is never read. A line longer than the buffer is drained to its end and refused
// (TooLong): its tail cannot become the start of a later answer. No thread is left behind on timeout, EOF or error. The source
// function must honour its timeout (the native stdin adapter in operator_input_reader.hpp does).
class OperatorLineReader {
public:
    OperatorLineReader(OperatorTimedRead read, void* user_data) noexcept : read_(read), user_data_(user_data) {}

    OperatorLineReader(const OperatorLineReader&) = delete;
    OperatorLineReader& operator=(const OperatorLineReader&) = delete;

    [[nodiscard]] OperatorLineResult read_once(std::chrono::milliseconds timeout) noexcept {
        if (used_) return OperatorLineResult{OperatorInputStatus::AlreadyRead};
        used_ = true;
        if (!read_) return OperatorLineResult{OperatorInputStatus::Error};
        if (timeout.count() <= 0) return OperatorLineResult{OperatorInputStatus::Timeout};
        // Bound both the duration arithmetic and a human confirmation's longest wait; a longer request is invalid input.
        if (timeout > std::chrono::minutes{5}) return OperatorLineResult{OperatorInputStatus::Error};

        const auto deadline = std::chrono::steady_clock::now() + timeout;
        OperatorLineResult result{};
        bool overlong = false;
        for (;;) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return OperatorLineResult{OperatorInputStatus::Timeout};
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            if (remaining.count() == 0) remaining = std::chrono::milliseconds{1};
            const OperatorByteResult next = read_(user_data_, remaining);
            if (std::chrono::steady_clock::now() >= deadline) return OperatorLineResult{OperatorInputStatus::Timeout};
            switch (next.status) {
                case OperatorByteStatus::Timeout: return OperatorLineResult{OperatorInputStatus::Timeout};
                case OperatorByteStatus::EndOfInput: return OperatorLineResult{OperatorInputStatus::EndOfInput};
                case OperatorByteStatus::Error: return OperatorLineResult{OperatorInputStatus::Error};
                case OperatorByteStatus::Byte: break;
            }
            if (next.byte == '\n' || next.byte == '\r') {
                result.status = overlong ? OperatorInputStatus::TooLong : OperatorInputStatus::Line;
                return result;
            }
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

// --- the verdict ----------------------------------------------------------------------------------------------------------------

enum class ConfirmationVerdict : std::uint8_t {
    NotAsked = 0,       // nobody judged anything: not a confirmation
    Confirmed = 1,      // a line arrived and it is exactly the phrase
    WrongPhrase = 2,    // a line arrived and it is anything else (case, a space, a different digest, the wrong uid ...)
    NoLine = 3,         // timeout, end of input, I/O error, an overlong line, a second read: see ConfirmationResult::input
    InvalidPhrase = 4,  // the challenge could not be turned into a phrase, so nothing can match it
};

struct ConfirmationResult {
    ConfirmationVerdict verdict{ConfirmationVerdict::NotAsked};
    OperatorInputStatus input{OperatorInputStatus::Error};
};

// Exactly the phrase, byte for byte, and nothing else: no trimming, no case folding, no "close enough".
inline ConfirmationResult judge_confirmation(const ConfirmationPhrase& phrase, const OperatorLineResult& input) noexcept {
    ConfirmationResult r{};
    r.input = input.status;
    if (!phrase.valid) {
        r.verdict = ConfirmationVerdict::InvalidPhrase;
    } else if (input.status != OperatorInputStatus::Line) {
        r.verdict = ConfirmationVerdict::NoLine;
    } else if (input.line() != phrase.view()) {
        r.verdict = ConfirmationVerdict::WrongPhrase;
    } else {
        r.verdict = ConfirmationVerdict::Confirmed;
    }
    return r;
}

}  // namespace hy::operator_resolution
