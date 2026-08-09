// SPDX-License-Identifier: proprietary
// fuzz_compaction_intent_codec.cpp — libFuzzer harness for Round D's
// compaction intent codec (docs/SPEC_INVARIANTS.md's "Seal-journal Round D"
// entry). Clang-only (HY_BUILD_FUZZ, needs -fsanitize=fuzzer); NOT part of
// the default ctest run -- see native/CMakeLists.txt's fuzz_compaction_
// intent_codec_smoke test, registered under LABELS "fuzz" with an explicit
// -runs/-timeout/-rss_limit_mb ceiling, invoked only via `ctest -L fuzz`.
//
// See compaction_intent_codec_fuzz_target.hpp for the actual target logic --
// this file is only the libFuzzer entry point, kept deliberately empty of
// anything that could drift from the GCC/MSVC corpus runner
// (test_compaction_intent_codec_corpus_runner.cpp) that exercises the exact
// same function.
#include "compaction_intent_codec_fuzz_target.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    hy::fuzz_target::compaction_intent_codec_one_input(data, size);
    return 0;
}
