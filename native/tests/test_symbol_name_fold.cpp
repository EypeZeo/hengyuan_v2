// SPDX-License-Identifier: proprietary
// test_symbol_name_fold.cpp -- D5 slice R3-0: what the control-plane log says `symbol_id` stands for.
//
// The audit log records an order's symbol only as a number. The operator tool (docs/OPERATOR_RESOLUTION_DESIGN.md 6) must put
// the exchange symbol into a query ("is there an order with this client order id on BTCUSDT?"), and a query for the WRONG
// symbol answers "no such order" for an order that exists -- the one mistake that turns an honest "absent" into a lie. The
// only durable source of the name is the symbol-registry snapshots ControlPlaneLogSink persists, one entry per id, keyed by
// POSITION. ControlPlaneLogSink::recovered_symbol_name() therefore answers Known only when EVERY snapshot of the log that
// mentions the id gives the same, well-formed name, and says Conflict (for good) the first time two of them disagree.
//
// These tests drive the real sink: snapshots are appended with append_snapshot() (which writes the name bytes exactly as
// given -- the wire format checks nothing about them), the sink is closed and a second one is opened over the same files, the
// way the offline tool will see them. A scenario that opens many stores in one test uses a fresh file name each time (see the
// fixture): deleting a log and creating a file of the same name a moment later races a real-time antivirus scanner on a
// Windows host (docs/SPEC_INVARIANTS.md; ControlPlaneLogSinkTest::use_fresh_base_path).

#include <gtest/gtest.h>
#include <hengyuan/control_plane_log_sink.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace hy;

namespace {

std::array<std::byte, kKeyBlockSize> make_plaintext_key(std::uint8_t fill) {
    std::array<std::byte, kKeyBlockSize> key{};
    for (std::size_t i = 0; i < key.size(); ++i) key[i] = static_cast<std::byte>(fill + i);
    return key;
}

std::string slurp_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

void spit_file(const std::string& path, const std::string& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// One registry entry whose name field holds exactly the given bytes (padded with NUL up to the field width, cut off at it).
// `name` may contain NULs and bytes that are no symbol at all: the wire format carries them as they are.
SymbolRules rule(std::string_view name) {
    SymbolRules r{};
    std::memcpy(r.symbol, name.data(), std::min(name.size(), kSymbolNameLen));
    r.is_trading = true;
    r.rules_version = 1;
    return r;
}

// The expected value of a name field, for a name given as a plain string.
constexpr std::string_view sv(const char* s) { return std::string_view(s); }

}  // namespace

class SymbolNameFoldTest : public ::testing::Test {
protected:
    std::string stem_;
    std::string base_path_;
    unsigned used_{0};
    std::unique_ptr<KeyRing> ring_;
    std::int64_t clock_ms_{1000};

    void SetUp() override {
        static int counter = 0;
        ++counter;
#ifdef _WIN32
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        stem_ = std::string(tmp) + "hy_symfold_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(counter);
#else
        stem_ = "/tmp/hy_symfold_" + std::to_string(getpid()) + "_" + std::to_string(counter);
#endif
        std::array<std::byte, kKekSize> kek{};
        for (std::size_t i = 0; i < kek.size(); ++i) kek[i] = static_cast<std::byte>(0x40 + i);
        ring_ = std::make_unique<KeyRing>(kek);
        WrappedKeyRecord rec{};
        ASSERT_EQ(ring_->add_key(1, make_plaintext_key(0x10), rec), KeyRingAddStatus::Ok);
        fresh_path();
    }

    void TearDown() override { remove_all(); }

    void remove_all() {
        if (base_path_.empty()) return;
        std::remove(base_path_.c_str());
        std::remove((base_path_ + ".cp.lock").c_str());
        std::remove((base_path_ + ".cp.tip").c_str());
        std::remove((base_path_ + ".cp.tip.tmp").c_str());
    }

    // A store of its own for every scenario (see the file header): the previous one is deleted, the name never reused.
    void fresh_path() {
        remove_all();
        base_path_ = stem_ + "_" + std::to_string(++used_) + ".log";
    }

    std::unique_ptr<ControlPlaneLogSink> open() { return std::make_unique<ControlPlaneLogSink>(base_path_, *ring_, 1); }

    // Appends one symbol-registry snapshot listing `names` (in id order) to the log, in a sink session of its own.
    void put(std::initializer_list<std::string_view> names, std::uint32_t rules_version = 1) {
        std::vector<SymbolRules> entries;
        for (const std::string_view n : names) entries.push_back(rule(n));
        put_rules(entries, rules_version);
    }

    void put_rules(const std::vector<SymbolRules>& entries, std::uint32_t rules_version = 1) {
        const auto sink = open();
        ASSERT_TRUE(sink->is_open());
        ASSERT_FALSE(sink->fenced());
        SymbolRegistrySnapshotPayload snap{};
        snap.timestamp_ms = clock_ms_;
        snap.rules_version = rules_version;
        snap.symbol_count = static_cast<std::uint32_t>(entries.size());
        clock_ms_ += 1000;
        ASSERT_TRUE(sink->append_snapshot(snap, std::span<const SymbolRules>(entries.data(), entries.size())).acked());
    }

    // What a freshly opened sink says the id is called.
    struct Said {
        SymbolNameStatus status{SymbolNameStatus::Unknown};
        std::string name;
    };
    static Said said(const ControlPlaneLogSink& sink, std::uint32_t symbol_id) {
        const SymbolNameLookup l = sink.recovered_symbol_name(symbol_id);
        return {l.status, std::string(l.name)};
    }

    void expect_known(const ControlPlaneLogSink& sink, std::uint32_t symbol_id, std::string_view name) {
        const Said s = said(sink, symbol_id);
        EXPECT_EQ(s.status, SymbolNameStatus::Known) << "id " << symbol_id;
        EXPECT_EQ(s.name, std::string(name)) << "id " << symbol_id;
    }
    void expect_conflict(const ControlPlaneLogSink& sink, std::uint32_t symbol_id) {
        const Said s = said(sink, symbol_id);
        EXPECT_EQ(s.status, SymbolNameStatus::Conflict) << "id " << symbol_id;
        EXPECT_TRUE(s.name.empty()) << "a Conflict names nothing: id " << symbol_id << " -> " << s.name;
    }
    void expect_unknown(const ControlPlaneLogSink& sink, std::uint32_t symbol_id) {
        const Said s = said(sink, symbol_id);
        EXPECT_EQ(s.status, SymbolNameStatus::Unknown) << "id " << symbol_id;
        EXPECT_TRUE(s.name.empty()) << "id " << symbol_id << " -> " << s.name;
    }
};

// --- the zero value is not an answer -----------------------------------------------------------------------------------------

TEST(SymbolNameLookupDefaults, ALookupNobodyFilledInIsUnknownAndNamesNothing) {
    const SymbolNameLookup l{};
    EXPECT_EQ(l.status, SymbolNameStatus::Unknown);
    EXPECT_TRUE(l.name.empty());
    EXPECT_NE(l.status, SymbolNameStatus::Known);
    EXPECT_EQ(static_cast<std::uint8_t>(SymbolNameStatus::Unknown), 0u);
}

TEST_F(SymbolNameFoldTest, AFreshLogKnowsNoNameAndCountsNoSnapshot) {
    const auto sink = open();
    ASSERT_EQ(sink->recovery_status(), RecoveryScanStatus::Clean);
    EXPECT_EQ(sink->recovered_snapshot_count(), 0u);
    for (const std::uint32_t id : {0u, 1u, 31u, 63u}) expect_unknown(*sink, id);
}

TEST_F(SymbolNameFoldTest, AnIdAtOrPastTheRegistryCapacityIsUnknownNotAMemoryRead) {
    // all 64 ids named, so the table is full: the id one past it must still answer Unknown
    std::vector<SymbolRules> entries;
    char buf[32];
    for (std::size_t i = 0; i < kMaxSymbols; ++i) {
        std::snprintf(buf, sizeof(buf), "SYM%02zu", i);
        entries.push_back(rule(buf));
    }
    put_rules(entries);
    const auto sink = open();
    ASSERT_EQ(sink->recovery_status(), RecoveryScanStatus::Recovered);
    for (const std::uint32_t id : {static_cast<std::uint32_t>(kMaxSymbols), static_cast<std::uint32_t>(kMaxSymbols) + 1,
                                   std::uint32_t{1000}, std::uint32_t{0x7FFFFFFFu}, std::uint32_t{0xFFFFFFFFu}}) {
        expect_unknown(*sink, id);
    }
}

// --- agreement ---------------------------------------------------------------------------------------------------------------

TEST_F(SymbolNameFoldTest, OneSnapshotNamesTheIdsItListsByPosition) {
    {
        std::vector<SymbolRules> entries{rule("BTCUSDT"), rule("ETHUSDT")};
        const auto sink = open();
        SymbolRegistrySnapshotPayload snap{};
        snap.timestamp_ms = 1000;
        snap.rules_version = 3;
        snap.symbol_count = 2;
        ASSERT_TRUE(sink->append_snapshot(snap, entries).acked());
        // the accessor is "what the log said when this sink was opened": the writing session's own append is not in it
        EXPECT_EQ(sink->recovered_snapshot_count(), 0u);
        expect_unknown(*sink, 0);
    }
    const auto sink = open();
    ASSERT_EQ(sink->recovery_status(), RecoveryScanStatus::Recovered);
    EXPECT_EQ(sink->recovered_snapshot_count(), 1u);
    expect_known(*sink, 0, "BTCUSDT");
    expect_known(*sink, 1, "ETHUSDT");
    expect_unknown(*sink, 2);
    expect_unknown(*sink, 63);
}

TEST_F(SymbolNameFoldTest, SnapshotsThatAgreeKeepTheNamesKnownWhateverElseTheyDifferIn) {
    put({"BTCUSDT", "ETHUSDT"}, 1);
    put({"BTCUSDT", "ETHUSDT"}, 2);          // a later refresh: another rules_version, the same names
    put({"BTCUSDT", "ETHUSDT", "SOLUSDT"}, 3);  // a symbol added at the end
    const auto sink = open();
    ASSERT_EQ(sink->recovery_status(), RecoveryScanStatus::Recovered);
    EXPECT_EQ(sink->recovered_snapshot_count(), 3u);
    expect_known(*sink, 0, "BTCUSDT");
    expect_known(*sink, 1, "ETHUSDT");
    expect_known(*sink, 2, "SOLUSDT");
    expect_unknown(*sink, 3);
}

TEST_F(SymbolNameFoldTest, ALaterSnapshotThatListsFewerIdsSaysNothingAboutTheOnesItLeavesOut) {
    // An order made under the first snapshot used id 2; the second snapshot not mentioning it does not unname it.
    put({"AAA1", "BBB2", "CCC3"});
    put({"AAA1"});
    const auto sink = open();
    ASSERT_EQ(sink->recovery_status(), RecoveryScanStatus::Recovered);
    EXPECT_EQ(sink->recovered_snapshot_count(), 2u);
    expect_known(*sink, 0, "AAA1");
    expect_known(*sink, 1, "BBB2");
    expect_known(*sink, 2, "CCC3");
}

TEST_F(SymbolNameFoldTest, AnEmptySnapshotIsCountedAndNamesNothing) {
    put({});
    const auto sink = open();
    ASSERT_EQ(sink->recovery_status(), RecoveryScanStatus::Recovered);
    EXPECT_EQ(sink->recovered_snapshot_count(), 1u);
    expect_unknown(*sink, 0);
}

TEST_F(SymbolNameFoldTest, AllSixtyFourIdsCanBeNamedAndEachKeepsItsOwnName) {
    std::vector<SymbolRules> entries;
    char buf[32];
    for (std::size_t i = 0; i < kMaxSymbols; ++i) {
        std::snprintf(buf, sizeof(buf), "SYM%02zu", i);
        entries.push_back(rule(buf));
    }
    put_rules(entries);
    put_rules(entries, 2);
    const auto sink = open();
    ASSERT_EQ(sink->recovery_status(), RecoveryScanStatus::Recovered);
    EXPECT_EQ(sink->recovered_snapshot_count(), 2u);
    for (std::size_t i = 0; i < kMaxSymbols; ++i) {
        std::snprintf(buf, sizeof(buf), "SYM%02zu", i);
        expect_known(*sink, static_cast<std::uint32_t>(i), buf);
    }
}

TEST_F(SymbolNameFoldTest, OtherFramesBetweenSnapshotsDoNotDisturbTheNamesOrTheCount) {
    put({"BTCUSDT", "ETHUSDT"});
    {
        const auto sink = open();
        EndpointWeightConfig weights{};
        weights.config_version = 4;
        ASSERT_TRUE(sink->append_weight_config(weights).acked());
        RateLimitUsageSnapshotPayload usage{};
        ASSERT_TRUE(sink->append_usage_snapshot(usage).acked());
        ASSERT_TRUE(sink->append_freeze_epoch_watermark(2, FrameTimeKind::ServerCorrectedUtc).acked());
    }
    put({"BTCUSDT", "ETHUSDT"}, 2);
    const auto sink = open();
    ASSERT_EQ(sink->recovery_status(), RecoveryScanStatus::Recovered);
    EXPECT_EQ(sink->recovered_snapshot_count(), 2u) << "only SymbolRegistrySnapshot frames are counted";
    expect_known(*sink, 0, "BTCUSDT");
    expect_known(*sink, 1, "ETHUSDT");
}

// --- disagreement ------------------------------------------------------------------------------------------------------------

TEST_F(SymbolNameFoldTest, ARenameMakesThatIdAConflictAndOnlyThatId) {
    put({"BTCUSDT", "ETHUSDT"});
    put({"BTCUSDC", "ETHUSDT"});
    const auto sink = open();
    ASSERT_EQ(sink->recovery_status(), RecoveryScanStatus::Recovered);
    expect_conflict(*sink, 0);
    expect_known(*sink, 1, "ETHUSDT");
}

TEST_F(SymbolNameFoldTest, ASwapOfTwoIdsMakesBothAConflict) {
    put({"BTCUSDT", "ETHUSDT"});
    put({"ETHUSDT", "BTCUSDT"});
    const auto sink = open();
    ASSERT_EQ(sink->recovery_status(), RecoveryScanStatus::Recovered);
    expect_conflict(*sink, 0);
    expect_conflict(*sink, 1);
}

TEST_F(SymbolNameFoldTest, ASymbolInsertedInFrontShiftsEveryIdAfterItIntoAConflict) {
    // Position is the key: put a symbol at the front and id 0 means something else, id 1 what id 0 used to mean.
    put({"BTCUSDT"});
    put({"AAVEUSDT", "BTCUSDT"});
    const auto sink = open();
    ASSERT_EQ(sink->recovery_status(), RecoveryScanStatus::Recovered);
    expect_conflict(*sink, 0);
    expect_known(*sink, 1, "BTCUSDT");
}

TEST_F(SymbolNameFoldTest, AConflictIsForGoodEvenWhenALaterSnapshotGoesBackToTheOriginalName) {
    put({"AAA1", "BBB2"});
    put({"CCC3", "BBB2"});
    put({"AAA1", "BBB2"});
    put({"AAA1", "BBB2"});
    const auto sink = open();
    ASSERT_EQ(sink->recovery_status(), RecoveryScanStatus::Recovered);
    EXPECT_EQ(sink->recovered_snapshot_count(), 4u);
    expect_conflict(*sink, 0);
    expect_known(*sink, 1, "BBB2");
}

// --- what counts as a name ---------------------------------------------------------------------------------------------------

TEST_F(SymbolNameFoldTest, OnlyAnUpperCaseLetterAndDigitNameTerminatedInsideTheFieldIsAName) {
    struct Case {
        const char* why;
        std::string field;  // the bytes of the name field, before padding
        bool good;
        const char* expect_name;  // when good
    };
    const std::string no_terminator(kSymbolNameLen, 'A');
    const std::string nineteen(kSymbolNameLen - 1, 'B');
    const Case cases[] = {
        {"an ordinary symbol", "BTCUSDT", true, "BTCUSDT"},
        {"a leading digit (1000SATSUSDT is a real symbol)", "1000SATSUSDT", true, "1000SATSUSDT"},
        {"one character", "X", true, "X"},
        {"nineteen characters, the longest that fits with its terminator", nineteen, true, nineteen.c_str()},
        {"padding bytes after the first NUL are padding, whatever they hold", std::string("BTC\0USDT", 8), true, "BTC"},
        {"no characters at all", "", false, ""},
        {"a name with no terminator inside the field", no_terminator, false, ""},
        {"lower case", "btcusdt", false, ""},
        {"one lower-case letter", "BTCUSDt", false, ""},
        {"a dash", "BTC-USDT", false, ""},
        {"an underscore", "BTC_USDT", false, ""},
        {"a space", "BTC USDT", false, ""},
        {"a trailing space", "BTCUSDT ", false, ""},
        {"the first letter and the first digit are fine", "AA00", true, "AA00"},
        {"the last letter and the last digit are fine", "ZZ99", true, "ZZ99"},
        {"the character just below 'A'", "BTC@USDT", false, ""},
        {"the character just above 'Z'", "BTC[USDT", false, ""},
        {"the character just below '0'", "BTC/USDT", false, ""},
        {"the character just above '9'", "BTC:USDT", false, ""},
        {"a control character", std::string("BTC\x01USDT"), false, ""},
        {"a non-ASCII byte", std::string("BTC\xC3\xA9USDT"), false, ""},
        {"a byte above 0x7F on its own", std::string("\xFF"), false, ""},
        {"a leading NUL (an empty name with something behind it)", std::string("\0BTC", 4), false, ""},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.why);
        fresh_path();
        put({std::string_view(c.field)});
        const auto sink = open();
        ASSERT_EQ(sink->recovery_status(), RecoveryScanStatus::Recovered);
        if (c.good) {
            expect_known(*sink, 0, c.expect_name);
        } else {
            expect_conflict(*sink, 0);
        }
    }
}

TEST_F(SymbolNameFoldTest, AMalformedNameIsAConflictWhetherItComesBeforeOrAfterAGoodOne) {
    {
        put({"BTCUSDT"});
        put({"btcusdt"});
        const auto sink = open();
        expect_conflict(*sink, 0);
    }
    fresh_path();
    {
        put({"btcusdt"});
        put({"BTCUSDT"});
        const auto sink = open();
        expect_conflict(*sink, 0);
    }
    fresh_path();
    {
        put({"BTCUSDT", ""});  // the second id has an empty name; the first is untouched
        const auto sink = open();
        expect_known(*sink, 0, "BTCUSDT");
        expect_conflict(*sink, 1);
    }
}

TEST_F(SymbolNameFoldTest, TheSameNameInTwoIdsOfOneSnapshotIsNotMergedAndNotRefused) {
    // The registry is not supposed to produce this; what the fold must not do is invent a link between the two ids.
    put({"BTCUSDT", "BTCUSDT"});
    const auto sink = open();
    ASSERT_EQ(sink->recovery_status(), RecoveryScanStatus::Recovered);
    expect_known(*sink, 0, "BTCUSDT");
    expect_known(*sink, 1, "BTCUSDT");
}

// --- a log that could not be read answers nothing ----------------------------------------------------------------------------

TEST_F(SymbolNameFoldTest, AFlippedByteInANameMakesTheLogCorruptAndEveryIdUnknownEvenForSnapshotsReadBeforeTheDamage) {
    // A tampered name is exactly what the MAC is for. The names folded from the frames BEFORE the damaged one must not
    // leak out of a recovery that failed.
    const std::size_t first_name_in_frame = detail::kControlPlaneHeaderSize + kSnapshotPayloadFixedWireSize;
    for (int damaged_frame = 0; damaged_frame < 2; ++damaged_frame) {
        SCOPED_TRACE(damaged_frame);
        fresh_path();
        put({"BTCUSDT", "ETHUSDT"});
        const std::size_t end0 = slurp_file(base_path_).size();
        put({"BTCUSDT", "ETHUSDT"}, 2);
        std::string bytes = slurp_file(base_path_);
        const std::size_t at = (damaged_frame == 0 ? 0 : end0) + first_name_in_frame + 1;
        ASSERT_LT(at, bytes.size());
        bytes[at] = static_cast<char>(bytes[at] ^ 0x20);  // 'T' -> 't' style lower-casing of one letter of the first name
        spit_file(base_path_, bytes);

        const auto sink = open();
        EXPECT_EQ(sink->recovery_status(), RecoveryScanStatus::Corrupt);
        EXPECT_TRUE(sink->fenced());
        for (const std::uint32_t id : {0u, 1u, 2u}) expect_unknown(*sink, id);
    }
}

TEST_F(SymbolNameFoldTest, ASecondSinkOverTheSameFilesCannotOpenAndKnowsNoName) {
    put({"BTCUSDT"});
    const auto first = open();
    ASSERT_EQ(first->recovery_status(), RecoveryScanStatus::Recovered);
    expect_known(*first, 0, "BTCUSDT");
    const auto second = open();  // the log is locked by the first
    EXPECT_NE(second->recovery_status(), RecoveryScanStatus::Recovered);
    EXPECT_NE(second->recovery_status(), RecoveryScanStatus::Clean);
    expect_unknown(*second, 0);
}

TEST_F(SymbolNameFoldTest, ATornTailIsCutAndItsNamesAreNeverCounted) {
    // A crash in the middle of the append of a second snapshot -- one that RENAMES id 0 -- leaves a prefix of its frame.
    // The first snapshot is intact: its names stand, and the partial frame says nothing.
    put({"BTCUSDT", "ETHUSDT"});
    const std::string anchor_after_first = slurp_file(base_path_ + ".cp.tip");
    const std::size_t end0 = slurp_file(base_path_).size();
    put({"BTCUSDC", "ETHUSDT"}, 2);
    const std::string full = slurp_file(base_path_);
    ASSERT_GT(full.size(), end0 + 120);

    for (const std::size_t torn : {std::size_t{1}, std::size_t{30}, std::size_t{50}, std::size_t{100}, full.size() - end0 - 1}) {
        SCOPED_TRACE(torn);
        const std::string anchor = anchor_after_first;
        const std::string log = full.substr(0, end0 + torn);
        fresh_path();
        spit_file(base_path_, log);
        spit_file(base_path_ + ".cp.tip", anchor);
        const auto sink = open();
        ASSERT_EQ(sink->recovery_status(), RecoveryScanStatus::Recovered);
        EXPECT_EQ(sink->recovered_torn_tail_bytes(), torn);
        EXPECT_EQ(sink->recovered_snapshot_count(), 1u);
        expect_known(*sink, 0, "BTCUSDT");
        expect_known(*sink, 1, "ETHUSDT");
    }
}

TEST_F(SymbolNameFoldTest, TheNameViewLivesAsLongAsTheSinkAndDoesNotMoveBetweenCalls) {
    put({"BTCUSDT", "ETHUSDT"});
    const auto sink = open();
    const SymbolNameLookup a = sink->recovered_symbol_name(0);
    const SymbolNameLookup b = sink->recovered_symbol_name(1);
    const SymbolNameLookup a_again = sink->recovered_symbol_name(0);
    ASSERT_EQ(a.status, SymbolNameStatus::Known);
    ASSERT_EQ(b.status, SymbolNameStatus::Known);
    EXPECT_EQ(a.name, sv("BTCUSDT"));
    EXPECT_EQ(b.name, sv("ETHUSDT"));
    EXPECT_EQ(a.name.data(), a_again.name.data()) << "a view into the sink, not a copy per call";
    EXPECT_NE(a.name.data(), b.name.data());
    EXPECT_EQ(a.name, sv("BTCUSDT")) << "asking for another id did not disturb the first view";
}
