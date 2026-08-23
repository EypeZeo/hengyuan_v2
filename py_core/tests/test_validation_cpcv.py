"""Unit tests for py_core.validation.cpcv (批次 2, round 2).

Two invariants are checked exhaustively across a parameter matrix, not just by example:
(1) cpcv_combinations()'s purge/embargo boundary math (same style as
test_validation_splits.py's purged-K-fold invariant, generalized to possibly-several
simultaneously-excluded test groups); (2) cpcv_paths()'s covering property -- every
(combination, group) test result produced by cpcv_combinations() is used in EXACTLY one
path, none missing, none duplicated. A fully hand-enumerated n_groups=6, n_test_groups=2
example (15 combinations, 5 paths, matching Lopez de Prado's own textbook worked example) is
also checked explicitly, since an exhaustive self-consistency check can only prove the
construction is internally coherent, not that it produces the specific assignment the module
docstring's derivation claims.
"""

from __future__ import annotations

import math

import pytest

from py_core.validation.cpcv import (
    _all_test_group_combinations,
    cpcv_combinations,
    cpcv_paths,
)

# ---------------------------------------------------------------------------
# cpcv_combinations(): exhaustive purge/embargo invariant
# ---------------------------------------------------------------------------


def _assert_purge_embargo_invariant(
    n_samples: int, combinations: list, *, purge_bars: int, embargo_bars: int
) -> None:
    for combo in combinations:
        train_set = set(combo.train_indices.tolist())
        all_test = set()
        for g in combo.test_groups:
            all_test |= set(combo.test_indices_by_group[g].tolist())
        assert train_set.isdisjoint(all_test), "train and test must never overlap"

        for g in combo.test_groups:
            test_idx = combo.test_indices_by_group[g]
            start = int(test_idx.min())
            end = int(test_idx.max()) + 1

            pre_purge = set(range(max(0, start - purge_bars), start))
            assert train_set.isdisjoint(pre_purge), f"pre-purge zone {pre_purge} leaked into train (group {g})"

            post_purge_end = min(n_samples, end + purge_bars)
            post_purge = set(range(end, post_purge_end))
            assert train_set.isdisjoint(post_purge), f"post-purge zone {post_purge} leaked into train (group {g})"

            embargo_end = min(n_samples, post_purge_end + embargo_bars)
            embargo = set(range(post_purge_end, embargo_end))
            assert train_set.isdisjoint(embargo), f"embargo zone {embargo} leaked into train (group {g})"


@pytest.mark.parametrize("n_samples", [24, 60])
@pytest.mark.parametrize("n_groups", [3, 4, 6])
@pytest.mark.parametrize("purge_bars", [0, 1, 2])
@pytest.mark.parametrize("embargo_bars", [0, 1])
def test_cpcv_combinations_invariant_holds_across_parameter_matrix(
    n_samples: int, n_groups: int, purge_bars: int, embargo_bars: int
) -> None:
    n_test_groups = 2 if n_groups > 2 else 1
    combos = cpcv_combinations(n_samples, n_groups, n_test_groups, purge_bars=purge_bars, embargo_bars=embargo_bars)
    assert len(combos) == math.comb(n_groups, n_test_groups)
    _assert_purge_embargo_invariant(n_samples, combos, purge_bars=purge_bars, embargo_bars=embargo_bars)


def test_cpcv_combinations_hand_verified_purge_and_embargo() -> None:
    # Hand-verified: n_samples=20, n_groups=4 (blocks of 5), n_test_groups=2, purge=1, embargo=1.
    combos = cpcv_combinations(20, 4, 2, purge_bars=1, embargo_bars=1)
    by_test_groups = {c.test_groups: c for c in combos}

    assert by_test_groups[(0, 1)].train_indices.tolist() == list(range(12, 20))
    assert by_test_groups[(0, 2)].train_indices.tolist() == [7, 8, 17, 18, 19]
    assert by_test_groups[(2, 3)].train_indices.tolist() == list(range(9))


def test_cpcv_combinations_test_groups_are_each_group_full_contiguous_block() -> None:
    combos = cpcv_combinations(30, 5, 2)
    for combo in combos:
        for g in combo.test_groups:
            diffs = combo.test_indices_by_group[g]
            assert len(diffs) > 0
            assert list(diffs) == list(range(int(diffs[0]), int(diffs[-1]) + 1)), "each test group must be contiguous"


def test_cpcv_combinations_aggressive_purge_can_empty_train_set() -> None:
    # Not an error here either (same documented stance as purged_kfold_splits()) -- it's
    # cpcv_analysis.py's job to detect and refuse an empty train set, not this function's.
    combos = cpcv_combinations(10, 4, 1, purge_bars=100, embargo_bars=100)
    assert any(len(c.train_indices) == 0 for c in combos)


def test_cpcv_combinations_rejects_n_groups_too_small() -> None:
    with pytest.raises(ValueError, match="n_groups"):
        cpcv_combinations(10, 1, 1)


def test_cpcv_combinations_rejects_n_test_groups_out_of_range() -> None:
    with pytest.raises(ValueError, match="n_test_groups"):
        cpcv_combinations(10, 4, 0)
    with pytest.raises(ValueError, match="n_test_groups"):
        cpcv_combinations(10, 4, 4)


def test_cpcv_combinations_rejects_negative_purge_or_embargo() -> None:
    with pytest.raises(ValueError, match="purge_bars"):
        cpcv_combinations(10, 4, 1, purge_bars=-1)
    with pytest.raises(ValueError, match="embargo_bars"):
        cpcv_combinations(10, 4, 1, embargo_bars=-1)


def test_cpcv_combinations_rejects_too_few_samples() -> None:
    with pytest.raises(ValueError, match="n_samples"):
        cpcv_combinations(3, 5, 1)


# ---------------------------------------------------------------------------
# cpcv_paths(): exhaustive covering property + hand-enumerated n_groups=6, k=2 example
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("n_groups", [2, 3, 4, 5, 6, 7])
def test_cpcv_paths_covering_property_holds_across_parameter_matrix(n_groups: int) -> None:
    for n_test_groups in range(1, n_groups):
        combos = _all_test_group_combinations(n_groups, n_test_groups)
        paths = cpcv_paths(n_groups, n_test_groups)

        assert len(paths) == math.comb(n_groups - 1, n_test_groups - 1)

        used: set[tuple[int, int]] = set()
        for path in paths:
            assert len(path.combination_for_group) == n_groups
            for g, combo_index in enumerate(path.combination_for_group):
                key = (combo_index, g)
                assert key not in used, f"(combination {combo_index}, group {g}) used by more than one path"
                used.add(key)

        expected = {(ci, g) for ci, combo in enumerate(combos) for g in combo}
        assert used == expected, f"missing={expected - used} extra={used - expected}"


def test_cpcv_paths_hand_verified_n6_k2() -> None:
    # Fully hand-enumerated (see cpcv.py module docstring / round-2 design notes):
    # combos in canonical order: (0,1) (0,2) (0,3) (0,4) (0,5) (1,2) (1,3) (1,4) (1,5)
    #                            (2,3) (2,4) (2,5) (3,4) (3,5) (4,5)  -- indices 0..14.
    paths = cpcv_paths(6, 2)
    assert len(paths) == 5
    assert [p.combination_for_group for p in paths] == [
        (0, 0, 1, 2, 3, 4),
        (1, 5, 5, 6, 7, 8),
        (2, 6, 9, 9, 10, 11),
        (3, 7, 10, 12, 12, 13),
        (4, 8, 11, 13, 14, 14),
    ]


def test_cpcv_paths_group_order_within_a_path_can_repeat_a_combination() -> None:
    # Not a bug: two groups sharing the SAME combination in the same path happens whenever
    # that combination is the canonically-earliest one testing both groups (see the n=6,k=2
    # hand-verified example -- path 0 uses combination 0 = test_groups (0,1) for BOTH group 0
    # and group 1). The covering-property test above is what actually proves correctness;
    # this test just documents that repeats within one path are expected, not a red flag.
    paths = cpcv_paths(6, 2)
    assert paths[0].combination_for_group[0] == paths[0].combination_for_group[1] == 0


def test_cpcv_paths_rejects_n_groups_too_small() -> None:
    with pytest.raises(ValueError, match="n_groups"):
        cpcv_paths(1, 1)


def test_cpcv_paths_rejects_n_test_groups_out_of_range() -> None:
    with pytest.raises(ValueError, match="n_test_groups"):
        cpcv_paths(4, 0)
    with pytest.raises(ValueError, match="n_test_groups"):
        cpcv_paths(4, 4)


def test_cpcv_combinations_and_cpcv_paths_agree_on_combination_count() -> None:
    for n_groups in (3, 4, 6):
        for n_test_groups in range(1, n_groups):
            combos = cpcv_combinations(40, n_groups, n_test_groups)
            paths = cpcv_paths(n_groups, n_test_groups)
            max_combo_index_used = max(ci for p in paths for ci in p.combination_for_group)
            assert max_combo_index_used < len(combos), "cpcv_paths() referenced a combination index cpcv_combinations() doesn't have"
