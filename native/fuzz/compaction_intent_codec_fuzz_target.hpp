// SPDX-License-Identifier: proprietary
// compaction_intent_codec_fuzz_target.hpp — the one fuzz-target function
// shared by both drivers of Round D's codec fuzzing (docs/SPEC_INVARIANTS.md's
// "Seal-journal Round D" entry):
//   - fuzz_compaction_intent_codec.cpp: a real libFuzzer harness (Clang-only,
//     HY_BUILD_FUZZ, LABELS "fuzz" -- not part of the default ctest run).
//   - tests/test_compaction_intent_codec_corpus_runner.cpp: a GCC/MSVC-
//     buildable deterministic runner over the checked-in seed corpus, no
//     libFuzzer dependency, part of the default ctest run.
// One definition so the two drivers cannot drift into testing different
// things -- same reasoning key_ring.hpp gives for its own constant_time_equal
// promotion (SEC-MACCMP-010).
//
// Governance: L1 (pure computation, calls only compaction_intent_codec.hpp's
// decode functions -- no file I/O of its own).

#pragma once

#include <hengyuan/compaction_intent_codec.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace hy::fuzz_target {

// Fixed, arbitrary key -- fuzzing decode() is about "never crashes/UB on
// attacker-controlled bytes for a GIVEN key," not about searching key space.
// The corpus's few "valid" seeds are pre-encoded against this exact key so
// they exercise the post-MAC-verification code paths too, not just the
// early-reject ones a random key would always hit.
inline std::array<std::byte, 32> fixed_key() noexcept {
    std::array<std::byte, 32> key{};
    for (std::size_t i = 0; i < key.size(); ++i) key[i] = static_cast<std::byte>(i + 1);
    return key;
}

// Feeds `data` to all three decode functions -- the full attacker-facing
// surface this round's codec exposes. Must never crash, UB, or leak
// (ASan/UBSan-clean); decode failures are expected and are not failures of
// this function itself.
inline void compaction_intent_codec_one_input(const std::uint8_t* data, std::size_t size) noexcept {
    const std::span<const std::byte> in(reinterpret_cast<const std::byte*>(data), size);
    const auto key = fixed_key();

    std::optional<VerifiedCompactionCandidateIntent> intent;
    (void)decode_compaction_candidate_intent_wire(in, key, intent);

    std::optional<VerifiedTransition> transition;
    (void)decode_compaction_intent_transition_wire(in, key, transition);

    std::optional<VerifiedGcAuthorized> gc;
    (void)decode_compaction_intent_gc_authorized_wire(in, key, gc);
}

}  // namespace hy::fuzz_target
