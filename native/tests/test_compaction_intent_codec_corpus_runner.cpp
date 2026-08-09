// GCC/MSVC-buildable deterministic runner over Round D's checked-in
// compaction-intent-codec seed corpus (native/fuzz/corpus/compaction_intent/)
// -- no libFuzzer/Clang dependency, part of the default ctest run. The real
// extended fuzzing loop (fuzz_compaction_intent_codec.cpp, Clang-only) is a
// separate, non-default `ctest -L fuzz` target; this test's job is just to
// make sure the checked-in seeds themselves never crash the decoder, on
// every toolchain this repo actually builds with day to day.
#include <gtest/gtest.h>

#include "../fuzz/compaction_intent_codec_fuzz_target.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#ifndef HY_COMPACTION_INTENT_CORPUS_DIR
#error "HY_COMPACTION_INTENT_CORPUS_DIR must be defined by CMake (see HY_TEST_FIXTURE_DIR for the established pattern)"
#endif

TEST(CompactionIntentCodecCorpusRunner, DecodeNeverCrashesOnSeedCorpus) {
    const std::filesystem::path corpus_dir(HY_COMPACTION_INTENT_CORPUS_DIR);
    ASSERT_TRUE(std::filesystem::exists(corpus_dir)) << corpus_dir;
    ASSERT_TRUE(std::filesystem::is_directory(corpus_dir)) << corpus_dir;

    std::size_t files_checked = 0;
    for (const auto& entry : std::filesystem::directory_iterator(corpus_dir)) {
        if (!entry.is_regular_file()) continue;

        std::ifstream in(entry.path(), std::ios::binary);
        ASSERT_TRUE(in) << entry.path();
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

        hy::fuzz_target::compaction_intent_codec_one_input(bytes.empty() ? nullptr : bytes.data(), bytes.size());
        ++files_checked;
    }

    EXPECT_GT(files_checked, 0u) << "corpus directory is empty -- this test would silently exercise nothing";
}
