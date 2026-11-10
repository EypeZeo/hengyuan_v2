// SPDX-License-Identifier: proprietary
// cxx_source_view.hpp -- the one scanner the source-scanning guards use to look at C++ source "as code".
//
// A guard test reads production files as text and counts or forbids constructs in them (the only mint of the readiness token,
// every assignment of OrchestratorContext::readiness, every call of drain_user_data_events(), the retired listenKey endpoints, ...).
// It has to look at the CODE: a mention in a comment, or a string that merely contains the words, must not count, and a real use
// must not hide behind either. strip_cxx_source() produces that view: comments removed and the contents of string, character and
// RAW string literals blanked (Literals::Blank, the code view) or kept (Literals::Keep, the literal view).
//
// Audit F-B-001 / F-B-010 / F-D-007: the scanner used to be copied into eight guards, and every copy knew only ordinary strings. A raw
// string literal -- R"(...)", R"x(...)x", with an optional u8 / u / U / L prefix -- ends at )delim" and may hold any number of double
// quotes, so one whose content has an ODD number of them left the scanner inside a "string" for the rest of the file: every protected
// construct after it vanished from the code view and the guard stayed green. The copies are now this one function, and it knows raw
// strings. (Still out of reach, as before: constructs spelled through the preprocessor -- #define CALL drain_user_data_events -- and
// text inside #if 0 blocks, which is scanned as if live.)
//
// Everything that is not a raw string behaves byte for byte as the copies did; test_cxx_source_view.cpp compares the two over the whole
// native/ tree to keep it so.

#pragma once

#include <cctype>
#include <cstddef>
#include <string>

namespace hy::test_helpers {

enum class Literals { Blank, Keep };

namespace detail {

[[nodiscard]] inline bool is_identifier_char(char c) noexcept {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// Does the '"' at s[quote] open a RAW string literal? It does when it directly follows an `R`, optionally with an encoding prefix
// (u8, u, U, L) in front of it, and that whole prefix starts a token -- `FOOR"x"` is the identifier FOOR followed by an ordinary string.
[[nodiscard]] inline bool opens_raw_string(const std::string& s, std::size_t quote) noexcept {
    if (quote == 0 || s[quote - 1] != 'R') return false;
    std::size_t start = quote - 1;  // the R
    if (start >= 2 && s[start - 2] == 'u' && s[start - 1] == '8') {
        start -= 2;
    } else if (start >= 1 && (s[start - 1] == 'u' || s[start - 1] == 'U' || s[start - 1] == 'L')) {
        start -= 1;
    }
    return start == 0 || !is_identifier_char(s[start - 1]);
}

// For a raw string whose opening quote is at s[quote]: the index of the `)` that closes it (the first `)delim"` after the opening
// `delim(`), and the delimiter's length. false when this is not a well-formed raw string opener -- no `(` within the 16 characters the
// standard allows for the delimiter, or a character in it that a delimiter cannot hold -- in which case the caller treats the quote as
// an ordinary string. An opener with no terminator runs to the end of the file (`close` == s.size()): nothing valid follows it anyway.
[[nodiscard]] inline bool find_raw_string_end(const std::string& s, std::size_t quote, std::size_t& open_paren, std::size_t& close,
                                              std::size_t& delimiter_len) noexcept {
    std::size_t p = quote + 1;
    std::size_t seen = 0;  // delimiter characters so far: the standard allows at most 16 before the `(`
    while (p < s.size() && seen < 16 && s[p] != '(') {
        const char c = s[p];
        if (c == ')' || c == '\\' || c == '"' || c == ' ' || c == '\t' || c == '\n' || c == '\r') return false;
        ++p;
        ++seen;
    }
    if (p >= s.size() || s[p] != '(') return false;
    open_paren = p;
    delimiter_len = p - (quote + 1);
    const std::string terminator = ")" + s.substr(quote + 1, delimiter_len) + "\"";
    const std::size_t at = s.find(terminator, p + 1);
    close = at == std::string::npos ? s.size() : at;
    return true;
}

}  // namespace detail

// Source text with comments removed; the contents of string, character and raw string literals either blanked (Literals::Blank) or
// kept (Literals::Keep). Newlines inside block comments and raw strings are kept so line structure survives.
[[nodiscard]] inline std::string strip_cxx_source(const std::string& s, Literals literals) {
    enum class State { Code, LineComment, BlockComment, String, Char };
    std::string out;
    out.reserve(s.size());
    State st = State::Code;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const char next = i + 1 < s.size() ? s[i + 1] : '\0';
        switch (st) {
            case State::Code:
                if (c == '/' && next == '/') {
                    st = State::LineComment;
                    ++i;
                } else if (c == '/' && next == '*') {
                    st = State::BlockComment;
                    ++i;
                } else if (c == '"') {
                    std::size_t open_paren = 0;
                    std::size_t close = 0;
                    std::size_t delimiter_len = 0;
                    if (detail::opens_raw_string(s, i) && detail::find_raw_string_end(s, i, open_paren, close, delimiter_len)) {
                        // R"delim( content )delim": no escapes inside, the content may hold any quote or comment marker.
                        out.push_back('"');
                        for (std::size_t k = open_paren + 1; k < close; ++k) {
                            if (literals == Literals::Keep || s[k] == '\n') out.push_back(s[k]);
                        }
                        out.push_back('"');
                        // Resume after the closing `)delim"`: i is set to the closing quote, the loop's ++i moves past it. An
                        // unterminated literal (close == s.size()) ends the scan.
                        i = close >= s.size() ? s.size() : close + delimiter_len + 1;
                    } else {
                        st = State::String;
                        out.push_back(c);
                    }
                } else if (c == '\'' && !out.empty() && detail::is_identifier_char(out.back())) {
                    out.push_back(c);  // a digit separator (1'000) or a literal prefix, not a char literal
                } else if (c == '\'') {
                    st = State::Char;
                    out.push_back(c);
                } else {
                    out.push_back(c);
                }
                break;
            case State::LineComment:
                if (c == '\n') {
                    st = State::Code;
                    out.push_back(c);
                }
                break;
            case State::BlockComment:
                if (c == '*' && next == '/') {
                    st = State::Code;
                    ++i;
                } else if (c == '\n') {
                    out.push_back(c);
                }
                break;
            case State::String:
            case State::Char: {
                const char quote = st == State::String ? '"' : '\'';
                if (c == '\\') {
                    if (literals == Literals::Keep) {
                        out.push_back(c);
                        if (i + 1 < s.size()) out.push_back(s[i + 1]);
                    }
                    ++i;  // skip the escaped character
                } else if (c == quote) {
                    st = State::Code;
                    out.push_back(c);
                } else if (literals == Literals::Keep) {
                    out.push_back(c);
                }
                break;
            }
        }
    }
    return out;
}

// The code view: comments and every literal's content gone. What the guards call code_only().
[[nodiscard]] inline std::string code_only(const std::string& s) { return strip_cxx_source(s, Literals::Blank); }

}  // namespace hy::test_helpers
