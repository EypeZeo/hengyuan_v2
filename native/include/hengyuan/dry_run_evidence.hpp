// SPDX-License-Identifier: proprietary
// dry_run_evidence.hpp — dry-run-before-live evidence chain.
//
// Governance: L1 (pure validation logic, no network, no secret).
// ADR-019 D11:
//   ✅ 4 mandatory evidence paths before live: submit, reject, ambiguity, kill
//   ✅ Evidence records with timestamps and checksums
//   ✅ All 4 paths must have evidence before live is unlocked
//   ✅ sim_executor simulation ≠ dry-run-before-live evidence

#pragma once

#include <cstdint>

namespace hy {

// --- Evidence path types (all 4 must be exercised before live) ---

enum class EvidencePath : std::uint8_t {
    SubmitSuccess = 0,   // Simulated submit → accepted → filled
    SubmitReject = 1,    // Simulated submit → rejected (pre-trade or exchange)
    SubmitAmbiguous = 2, // Simulated timeout → ambiguous → reconcile
    KillSwitch = 3,      // Kill switch trigger → fail-closed → no new orders
};

static constexpr std::size_t kEvidencePathCount = 4;

// --- Evidence record ---

struct EvidenceRecord {
    bool exercised{false};
    std::int64_t timestamp_ms{0};
    std::uint32_t build_hash{0};     // git commit hash (lower 32 bits)
    std::uint32_t test_suite_id{0};  // which test produced this evidence
};

// --- Evidence chain ---

class DryRunEvidenceChain {
public:
    void record(EvidencePath path, std::int64_t ts_ms,
                std::uint32_t build_hash, std::uint32_t suite_id) noexcept {
        auto idx = static_cast<std::size_t>(path);
        if (idx >= kEvidencePathCount) return;
        evidence_[idx].exercised = true;
        evidence_[idx].timestamp_ms = ts_ms;
        evidence_[idx].build_hash = build_hash;
        evidence_[idx].test_suite_id = suite_id;
    }

    bool is_path_exercised(EvidencePath path) const noexcept {
        auto idx = static_cast<std::size_t>(path);
        if (idx >= kEvidencePathCount) return false;
        return evidence_[idx].exercised;
    }

    const EvidenceRecord& get(EvidencePath path) const noexcept {
        return evidence_[static_cast<std::size_t>(path)];
    }

    // All 4 paths must be exercised before live is permitted.
    bool all_paths_exercised() const noexcept {
        for (std::size_t i = 0; i < kEvidencePathCount; ++i) {
            if (!evidence_[i].exercised) return false;
        }
        return true;
    }

    // Count how many paths have evidence.
    std::uint32_t exercised_count() const noexcept {
        std::uint32_t n = 0;
        for (std::size_t i = 0; i < kEvidencePathCount; ++i) {
            if (evidence_[i].exercised) ++n;
        }
        return n;
    }

    // All evidence must come from the same build to be valid.
    bool consistent_build() const noexcept {
        std::uint32_t first_hash = 0;
        bool found_first = false;
        for (std::size_t i = 0; i < kEvidencePathCount; ++i) {
            if (!evidence_[i].exercised) continue;
            if (!found_first) {
                first_hash = evidence_[i].build_hash;
                found_first = true;
            } else if (evidence_[i].build_hash != first_hash) {
                return false;
            }
        }
        return true;
    }

    // Final gate: can we proceed to live?
    bool live_ready() const noexcept {
        return all_paths_exercised() && consistent_build();
    }

    void reset() noexcept {
        for (auto& e : evidence_) e = {};
    }

private:
    EvidenceRecord evidence_[kEvidencePathCount]{};
};

}  // namespace hy
