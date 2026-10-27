// SPDX-License-Identifier: proprietary
// test_operator_resolution_confirmation.cpp -- D5 slice R3a: the phrase the operator types, the line reader that takes it and the
// judge that compares the two (operator_resolution_confirmation.hpp).
//
// What these pin: the phrase is EXACTLY "ABSENT <coid> <symbol> <ENVIRONMENT> <uid> <8 digest characters>" and nothing about it
// is forgiving; every field outside its grammar makes the whole phrase invalid (so nothing can match it); the line reader reads
// one line, once, drains an overlong one to its end and refuses it, and reports a timeout, an end of input and an I/O error as
// what they are; and the judge says Confirmed only for a Line that equals the phrase byte for byte.

#include <gtest/gtest.h>
#include <hengyuan/operator_resolution_confirmation.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace hy;
using namespace hy::operator_resolution;

namespace {

constexpr std::string_view kDigest = "3f9a0c1e5b7d2468ace013579bdf02468ace13579bdf02468ace13579bdf0246";  // 64 lower-case hex

ConfirmationChallenge good_challenge() {
    ConfirmationChallenge c;
    c.client_order_id = "HY-1790000000000-7-0A1B";
    c.symbol = "BTCUSDT";
    c.environment = OperatorEnvironment::Testnet;
    c.account_uid = 123456789;
    c.evidence_digest_hex = kDigest;
    return c;
}

// A scripted byte source: the bytes of `text`, then whatever `then` says.
struct Script {
    std::string text;
    std::size_t pos{0};
    OperatorByteStatus then{OperatorByteStatus::EndOfInput};
    int calls{0};
};

OperatorByteResult scripted(void* user, std::chrono::milliseconds) noexcept {
    auto* s = static_cast<Script*>(user);
    ++s->calls;
    if (s->pos < s->text.size()) return {OperatorByteStatus::Byte, s->text[s->pos++]};
    return {s->then, 0};
}

OperatorLineResult read_text(const std::string& text, OperatorByteStatus then = OperatorByteStatus::EndOfInput) {
    Script s{text, 0, then, 0};
    OperatorLineReader reader(&scripted, &s);
    return reader.read_once(std::chrono::seconds{5});
}

// A byte source that takes `delay` to answer, whatever the timeout it is given (a source that does not honour its timeout).
struct SlowScript {
    std::string text;
    std::size_t pos{0};
    std::chrono::milliseconds delay{0};
};

OperatorByteResult slow_scripted(void* user, std::chrono::milliseconds) noexcept {
    auto* s = static_cast<SlowScript*>(user);
    std::this_thread::sleep_for(s->delay);
    if (s->pos < s->text.size()) return {OperatorByteStatus::Byte, s->text[s->pos++]};
    return {OperatorByteStatus::EndOfInput, 0};
}

}  // namespace

// --- grammars -------------------------------------------------------------------------------------------------------------------

TEST(OperatorResolutionConfirmation, ClientOrderIdGrammarIsTheExchangesAndFitsTheAuditField) {
    EXPECT_TRUE(is_exchange_client_order_id("HY-1790000000000-7-0A1B"));
    EXPECT_TRUE(is_exchange_client_order_id("a"));
    EXPECT_TRUE(is_exchange_client_order_id("A.b:c/d_e-f9"));
    EXPECT_TRUE(is_exchange_client_order_id(std::string(kClientOrderIdLen, 'x')));
    EXPECT_FALSE(is_exchange_client_order_id(""));
    EXPECT_FALSE(is_exchange_client_order_id(std::string(kClientOrderIdLen + 1, 'x')));
    for (const char bad : {' ', '\t', '\n', '"', '\'', '\\', '&', '=', '?', '%', '+', '#', ';', ',', '*', '(', '\x01', '\x7F', '\x80', '\xFF'}) {
        EXPECT_FALSE(is_exchange_client_order_id(std::string("AB") + bad + "CD")) << static_cast<int>(static_cast<unsigned char>(bad));
    }
}

TEST(OperatorResolutionConfirmation, SymbolGrammarIsTheOneTheControlPlaneFoldAccepts) {
    EXPECT_TRUE(is_exchange_symbol("BTCUSDT"));
    EXPECT_TRUE(is_exchange_symbol("1000SATSUSDT"));
    EXPECT_TRUE(is_exchange_symbol(std::string(kSymbolNameLen - 1, 'A')));
    EXPECT_FALSE(is_exchange_symbol(""));
    EXPECT_FALSE(is_exchange_symbol(std::string(kSymbolNameLen, 'A')));
    for (const char bad : {'a', 'z', '-', '_', ' ', '@', '[', '/', ':', '\0', '\x80'}) {
        EXPECT_FALSE(is_exchange_symbol(std::string("BTC") + bad + "USDT")) << static_cast<int>(static_cast<unsigned char>(bad));
    }
}

TEST(OperatorResolutionConfirmation, OperatorLabelFitsTheFramesDetailMessage) {
    EXPECT_TRUE(is_operator_label("alice"));
    EXPECT_TRUE(is_operator_label("ops.team_2-b"));
    EXPECT_TRUE(is_operator_label(std::string(kOperatorLabelMaxChars, 'a')));
    EXPECT_FALSE(is_operator_label(""));
    EXPECT_FALSE(is_operator_label(std::string(kOperatorLabelMaxChars + 1, 'a')));
    EXPECT_FALSE(is_operator_label("a b"));
    EXPECT_FALSE(is_operator_label("a;b"));  // ';' separates the fields of detail_msg
    EXPECT_FALSE(is_operator_label("a=b"));
    // "op=<label>;ev=<32 hex>" in a 63-character field, whatever the label
    EXPECT_EQ(std::string("op=").size() + kOperatorLabelMaxChars + std::string(";ev=").size() + 32, 63u);
}

TEST(OperatorResolutionConfirmation, LowerHexMeansExactlyThatManyLowerCaseHexCharacters) {
    EXPECT_TRUE(is_lower_hex(kDigest, kEvidenceDigestHexChars));
    EXPECT_FALSE(is_lower_hex(kDigest.substr(0, 63), kEvidenceDigestHexChars));
    EXPECT_FALSE(is_lower_hex(std::string(kDigest) + "0", kEvidenceDigestHexChars));
    std::string upper(kDigest);
    upper[10] = 'A';
    EXPECT_FALSE(is_lower_hex(upper, kEvidenceDigestHexChars));
    std::string non_hex(kDigest);
    non_hex[63] = 'g';
    EXPECT_FALSE(is_lower_hex(non_hex, kEvidenceDigestHexChars));
}

// Every one of the 256 byte values against a LITERAL list of what each grammar allows: the classes are spelled out here, not
// computed, so a range boundary that is off by one in the grammar cannot be mirrored by the test.
TEST(OperatorResolutionConfirmation, EveryByteValueIsJudgedAgainstALiteralListOfWhatEachGrammarAllows) {
    const std::string digits = "0123456789";
    const std::string upper = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    const std::string lower = "abcdefghijklmnopqrstuvwxyz";
    const std::string coid_chars = digits + upper + lower + ".:/_-";
    const std::string symbol_chars = digits + upper;
    const std::string label_chars = digits + upper + lower + "._-";
    const std::string hex_chars = digits + "abcdef";
    for (int v = 0; v < 256; ++v) {
        const char ch = static_cast<char>(v);
        const std::string one(1, ch);
        const auto allowed = [&](const std::string& set) { return v != 0 && set.find(ch) != std::string::npos; };
        EXPECT_EQ(is_exchange_client_order_id(one), allowed(coid_chars)) << "client order id, byte " << v;
        EXPECT_EQ(is_exchange_symbol(one), allowed(symbol_chars)) << "symbol, byte " << v;
        EXPECT_EQ(is_operator_label(one), allowed(label_chars)) << "label, byte " << v;
        EXPECT_EQ(is_lower_hex(one, 1), allowed(hex_chars)) << "lower hex, byte " << v;
    }
}

TEST(OperatorResolutionConfirmation, EnvironmentNamesAreTheTwoAndNothingForAnythingElse) {
    EXPECT_EQ(environment_name(OperatorEnvironment::Testnet), "TESTNET");
    EXPECT_EQ(environment_name(OperatorEnvironment::Production), "PRODUCTION");
    EXPECT_TRUE(environment_name(static_cast<OperatorEnvironment>(255)).empty());
    EXPECT_TRUE(environment_name(static_cast<OperatorEnvironment>(2)).empty());
}

// --- the phrase -----------------------------------------------------------------------------------------------------------------

TEST(OperatorResolutionConfirmation, ThePhraseIsExactlyTheDocumentedLine) {
    const ConfirmationPhrase p = make_confirmation_phrase(good_challenge());
    ASSERT_TRUE(p.valid);
    EXPECT_EQ(p.view(), "ABSENT HY-1790000000000-7-0A1B BTCUSDT TESTNET 123456789 3f9a0c1e");
    EXPECT_EQ(p.size, p.view().size());
    ConfirmationChallenge prod = good_challenge();
    prod.environment = OperatorEnvironment::Production;
    EXPECT_EQ(make_confirmation_phrase(prod).view(), "ABSENT HY-1790000000000-7-0A1B BTCUSDT PRODUCTION 123456789 3f9a0c1e");
}

TEST(OperatorResolutionConfirmation, TheDecimalAccountUidIsWrittenInFull) {
    for (const auto& [uid, text] : {std::pair<std::uint64_t, const char*>{1, "1"},
                                    {9, "9"},
                                    {10, "10"},
                                    {99, "99"},
                                    {100, "100"},
                                    {4294967295ULL, "4294967295"},
                                    {4294967296ULL, "4294967296"},
                                    {18446744073709551615ULL, "18446744073709551615"}}) {
        ConfirmationChallenge c = good_challenge();
        c.account_uid = uid;
        const ConfirmationPhrase p = make_confirmation_phrase(c);
        ASSERT_TRUE(p.valid) << uid;
        EXPECT_EQ(p.view(), std::string("ABSENT HY-1790000000000-7-0A1B BTCUSDT TESTNET ") + text + " 3f9a0c1e");
    }
}

TEST(OperatorResolutionConfirmation, OnlyTheFirstEightDigestCharactersAreTyped) {
    ConfirmationChallenge a = good_challenge();
    ConfirmationChallenge b = good_challenge();
    std::string other(kDigest);
    other[8] = other[8] == '0' ? '1' : '0';  // differs after the typed prefix
    b.evidence_digest_hex = other;
    EXPECT_EQ(make_confirmation_phrase(a).view(), make_confirmation_phrase(b).view());
    std::string within(kDigest);
    within[7] = within[7] == '0' ? '1' : '0';  // differs inside the prefix
    b.evidence_digest_hex = within;
    EXPECT_NE(make_confirmation_phrase(a).view(), make_confirmation_phrase(b).view());
}

TEST(OperatorResolutionConfirmation, EveryFieldOutsideItsGrammarMakesTheWholePhraseInvalid) {
    const auto invalid = [](const ConfirmationChallenge& c) {
        const ConfirmationPhrase p = make_confirmation_phrase(c);
        EXPECT_FALSE(p.valid);
        EXPECT_TRUE(p.view().empty());
        EXPECT_EQ(p.size, 0u);
    };
    ConfirmationChallenge c = good_challenge();
    c.client_order_id = "";
    invalid(c);
    c = good_challenge();
    c.client_order_id = "HY 1";
    invalid(c);
    c = good_challenge();
    c.symbol = "btcusdt";
    invalid(c);
    c = good_challenge();
    c.symbol = "";
    invalid(c);
    c = good_challenge();
    c.environment = static_cast<OperatorEnvironment>(255);
    invalid(c);
    c = good_challenge();
    c.account_uid = 0;
    invalid(c);
    c = good_challenge();
    c.evidence_digest_hex = "";
    invalid(c);
    c = good_challenge();
    c.evidence_digest_hex = kDigest.substr(0, 63);
    invalid(c);
    c = good_challenge();
    EXPECT_TRUE(make_confirmation_phrase(c).valid) << "the unmodified challenge is valid";
    EXPECT_FALSE(ConfirmationPhrase{}.valid) << "a phrase nobody built is not valid";
    EXPECT_TRUE(ConfirmationPhrase{}.view().empty());
}

TEST(OperatorResolutionConfirmation, TheLongestPhraseFitsAndTheWriterRefusesToOverflow) {
    ConfirmationChallenge c = good_challenge();
    const std::string longest(kClientOrderIdLen, 'X');
    const std::string longest_symbol(kSymbolNameLen - 1, 'Z');
    c.client_order_id = longest;
    c.symbol = longest_symbol;
    c.environment = OperatorEnvironment::Production;
    c.account_uid = 18446744073709551615ULL;
    const ConfirmationPhrase p = make_confirmation_phrase(c);
    ASSERT_TRUE(p.valid);
    EXPECT_LT(p.size, kConfirmationLineMaxBytes);
    // the writer itself: one byte too many is a refusal, and it stays one
    std::array<char, kConfirmationLineMaxBytes> buf{};
    operator_resolution::detail::PhraseWriter w{buf};
    w.put(std::string(kConfirmationLineMaxBytes, 'a'));
    EXPECT_TRUE(w.ok);
    EXPECT_EQ(w.size, kConfirmationLineMaxBytes);
    w.put("b");
    EXPECT_FALSE(w.ok);
    EXPECT_EQ(w.size, kConfirmationLineMaxBytes) << "nothing was written past the end";
    w.put("");
    EXPECT_FALSE(w.ok) << "a failed writer stays failed";
}

// --- the line reader ---------------------------------------------------------------------------------------------------------------

TEST(OperatorResolutionConfirmation, ALineEndsAtLfOrCrAndKeepsEverythingElse) {
    OperatorLineResult r = read_text("hello world\n");
    EXPECT_EQ(r.status, OperatorInputStatus::Line);
    EXPECT_EQ(r.line(), "hello world");
    r = read_text("hello\r\n");
    EXPECT_EQ(r.status, OperatorInputStatus::Line);
    EXPECT_EQ(r.line(), "hello") << "the line ends at the CR; the LF behind it is never read";
    r = read_text("\n");
    EXPECT_EQ(r.status, OperatorInputStatus::Line);
    EXPECT_TRUE(r.line().empty());
    r = read_text("  padded  \n");
    EXPECT_EQ(r.line(), "  padded  ") << "no trimming";
}

TEST(OperatorResolutionConfirmation, OnlyTheFirstLineIsReadAndTheReaderIsOneShot) {
    Script s{"first\nsecond\n", 0, OperatorByteStatus::EndOfInput, 0};
    OperatorLineReader reader(&scripted, &s);
    const OperatorLineResult a = reader.read_once(std::chrono::seconds{5});
    EXPECT_EQ(a.line(), "first");
    const int calls_after_first = s.calls;
    const OperatorLineResult b = reader.read_once(std::chrono::seconds{5});
    EXPECT_EQ(b.status, OperatorInputStatus::AlreadyRead);
    EXPECT_TRUE(b.line().empty());
    EXPECT_EQ(s.calls, calls_after_first) << "a second read does not touch the source";
}

TEST(OperatorResolutionConfirmation, ALineOfExactlyTheBufferSizeIsAcceptedOneMoreByteIsDrainedAndRefused) {
    const std::string full(kConfirmationLineMaxBytes, 'a');
    OperatorLineResult r = read_text(full + "\n");
    EXPECT_EQ(r.status, OperatorInputStatus::Line);
    EXPECT_EQ(r.line(), full);

    Script s{full + "bcd\nrest\n", 0, OperatorByteStatus::EndOfInput, 0};
    OperatorLineReader reader(&scripted, &s);
    r = reader.read_once(std::chrono::seconds{5});
    EXPECT_EQ(r.status, OperatorInputStatus::TooLong);
    EXPECT_EQ(s.text[s.pos - 1], '\n') << "drained up to and including the end of the long line";
    EXPECT_EQ(s.pos, kConfirmationLineMaxBytes + 4) << "and not a byte of the next line";
}

TEST(OperatorResolutionConfirmation, ATimeoutAnEndOfInputAndAnErrorAreWhatTheyAre) {
    EXPECT_EQ(read_text("partial", OperatorByteStatus::Timeout).status, OperatorInputStatus::Timeout);
    EXPECT_EQ(read_text("partial", OperatorByteStatus::EndOfInput).status, OperatorInputStatus::EndOfInput);
    EXPECT_EQ(read_text("partial", OperatorByteStatus::Error).status, OperatorInputStatus::Error);
    EXPECT_EQ(read_text("", OperatorByteStatus::Timeout).status, OperatorInputStatus::Timeout);
    EXPECT_EQ(read_text("", OperatorByteStatus::EndOfInput).status, OperatorInputStatus::EndOfInput);
    EXPECT_EQ(read_text("", OperatorByteStatus::Error).status, OperatorInputStatus::Error);
}

TEST(OperatorResolutionConfirmation, AByteThatArrivesAfterTheDeadlineIsNotAnAnswerNotEvenTheOneThatEndsTheLine) {
    // the source takes 80 ms to hand over a newline although it was given 20 ms: the line it would end is not an answer
    SlowScript late{"\n", 0, std::chrono::milliseconds{80}};
    OperatorLineReader reader(&slow_scripted, &late);
    EXPECT_EQ(reader.read_once(std::chrono::milliseconds{20}).status, OperatorInputStatus::Timeout);
    // and the same source in time is a line
    SlowScript prompt{"ok\n", 0, std::chrono::milliseconds{0}};
    OperatorLineReader in_time(&slow_scripted, &prompt);
    const OperatorLineResult r = in_time.read_once(std::chrono::seconds{5});
    EXPECT_EQ(r.status, OperatorInputStatus::Line);
    EXPECT_EQ(r.line(), "ok");
}

TEST(OperatorResolutionConfirmation, ABadSourceOrABadTimeoutIsNeverALine) {
    OperatorLineReader no_source(nullptr, nullptr);
    EXPECT_EQ(no_source.read_once(std::chrono::seconds{5}).status, OperatorInputStatus::Error);

    Script s{"x\n", 0, OperatorByteStatus::EndOfInput, 0};
    OperatorLineReader zero(&scripted, &s);
    EXPECT_EQ(zero.read_once(std::chrono::milliseconds{0}).status, OperatorInputStatus::Timeout);
    OperatorLineReader negative(&scripted, &s);
    EXPECT_EQ(negative.read_once(std::chrono::milliseconds{-1}).status, OperatorInputStatus::Timeout);
    OperatorLineReader too_long_a_wait(&scripted, &s);
    EXPECT_EQ(too_long_a_wait.read_once(std::chrono::minutes{5} + std::chrono::milliseconds{1}).status, OperatorInputStatus::Error);
    EXPECT_EQ(s.calls, 0) << "none of the three asked the source anything";
    OperatorLineReader exactly_five_minutes(&scripted, &s);
    EXPECT_EQ(exactly_five_minutes.read_once(std::chrono::minutes{5}).status, OperatorInputStatus::Line);
}

TEST(OperatorResolutionConfirmation, ADefaultLineResultIsNotALine) {
    const OperatorLineResult r{};
    EXPECT_EQ(r.status, OperatorInputStatus::Error);
    EXPECT_TRUE(r.line().empty());
}

// --- the judge ------------------------------------------------------------------------------------------------------------------

namespace {

OperatorLineResult typed(const std::string& s) {
    OperatorLineResult r{};
    r.status = OperatorInputStatus::Line;
    r.size = s.size() < r.bytes.size() ? s.size() : r.bytes.size();
    for (std::size_t i = 0; i < r.size; ++i) r.bytes[i] = s[i];
    return r;
}

}  // namespace

TEST(OperatorResolutionConfirmation, OnlyTheExactPhraseConfirms) {
    const ConfirmationPhrase p = make_confirmation_phrase(good_challenge());
    ASSERT_TRUE(p.valid);
    const std::string exact(p.view());
    EXPECT_EQ(judge_confirmation(p, typed(exact)).verdict, ConfirmationVerdict::Confirmed);

    std::vector<std::string> wrong;
    wrong.push_back("");
    wrong.push_back(exact + " ");
    wrong.push_back(" " + exact);
    wrong.push_back(exact + "x");
    wrong.push_back(exact.substr(0, exact.size() - 1));
    std::string lower = exact;
    lower[0] = 'a';
    wrong.push_back(lower);
    std::string other_digest = exact;
    other_digest.back() = other_digest.back() == '0' ? '1' : '0';
    wrong.push_back(other_digest);
    std::string other_uid = exact;
    other_uid[other_uid.find("123456789")] = '9';
    wrong.push_back(other_uid);
    std::string other_env = exact;
    wrong.push_back(other_env.replace(other_env.find("TESTNET"), 7, "PRODUCT"));
    wrong.push_back("CONFIRM");
    for (const std::string& line : wrong) {
        const ConfirmationResult r = judge_confirmation(p, typed(line));
        EXPECT_EQ(r.verdict, ConfirmationVerdict::WrongPhrase) << '[' << line << ']';
        EXPECT_EQ(r.input, OperatorInputStatus::Line);
    }
}

TEST(OperatorResolutionConfirmation, EverythingThatIsNotALineIsNoLine) {
    const ConfirmationPhrase p = make_confirmation_phrase(good_challenge());
    for (const OperatorInputStatus st : {OperatorInputStatus::Timeout, OperatorInputStatus::EndOfInput, OperatorInputStatus::Error,
                                         OperatorInputStatus::TooLong, OperatorInputStatus::AlreadyRead}) {
        OperatorLineResult line = typed(std::string(p.view()));  // even with the right bytes in the buffer
        line.status = st;
        const ConfirmationResult r = judge_confirmation(p, line);
        EXPECT_EQ(r.verdict, ConfirmationVerdict::NoLine) << static_cast<int>(st);
        EXPECT_EQ(r.input, st);
    }
}

TEST(OperatorResolutionConfirmation, AnInvalidPhraseCanNeverBeConfirmedNotEvenByAnEmptyLine) {
    const ConfirmationPhrase none{};
    EXPECT_EQ(judge_confirmation(none, typed("")).verdict, ConfirmationVerdict::InvalidPhrase);
    EXPECT_EQ(judge_confirmation(none, typed("ABSENT")).verdict, ConfirmationVerdict::InvalidPhrase);
    EXPECT_EQ(judge_confirmation(none, OperatorLineResult{}).verdict, ConfirmationVerdict::InvalidPhrase);
}

TEST(OperatorResolutionConfirmation, ANeverJudgedResultIsNotAConfirmation) {
    const ConfirmationResult r{};
    EXPECT_EQ(r.verdict, ConfirmationVerdict::NotAsked);
    EXPECT_NE(r.verdict, ConfirmationVerdict::Confirmed);
    EXPECT_EQ(static_cast<std::uint8_t>(ConfirmationVerdict::NotAsked), 0u);
}
