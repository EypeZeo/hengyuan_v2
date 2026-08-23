"""Unit tests for py_core.validation.splits (批次 2, round 1).

The core invariant every split from either function must satisfy -- no train index within
purge_bars of any test index on either side, and no train index within embargo_bars
immediately after a test fold's post-purge boundary -- is checked exhaustively for every
generated split, not asserted by a handful of hand-picked examples. A handful of the
smallest cases are also hand-verified explicitly (see the comments), since an exhaustive
check can itself only prove internal self-consistency, not that the boundary math is placed
where the module docstring claims it is.
"""

from __future__ import annotations

from itertools import pairwise

import numpy as np
import pytest

from py_core.validation.splits import Split, purged_kfold_splits, walk_forward_splits


def _assert_purge_embargo_invariant(
    n_samples: int, splits: list[Split], *, purge_bars: int, embargo_bars: int
) -> None:
    for split in splits:
        train_set = set(split.train_indices.tolist())
        test_set = set(split.test_indices.tolist())
        assert train_set.isdisjoint(test_set), "train and test must never overlap"

        test_start = int(split.test_indices.min())
        test_end = int(split.test_indices.max()) + 1

        pre_purge = set(range(max(0, test_start - purge_bars), test_start))
        assert train_set.isdisjoint(pre_purge), f"pre-purge zone {pre_purge} leaked into train"

        post_purge_end = min(n_samples, test_end + purge_bars)
        post_purge = set(range(test_end, post_purge_end))
        assert train_set.isdisjoint(post_purge), f"post-purge zone {post_purge} leaked into train"

        embargo_end = min(n_samples, post_purge_end + embargo_bars)
        embargo = set(range(post_purge_end, embargo_end))
        assert train_set.isdisjoint(embargo), f"embargo zone {embargo} leaked into train"


# ---------------------------------------------------------------------------
# purged_kfold_splits(): exhaustive invariant + hand-verified small cases
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("n_samples", [20, 37, 100])
@pytest.mark.parametrize("n_splits", [2, 3, 5, 7])
@pytest.mark.parametrize("purge_bars", [0, 1, 3])
@pytest.mark.parametrize("embargo_bars", [0, 1, 4])
def test_purged_kfold_invariant_holds_across_parameter_matrix(
    n_samples: int, n_splits: int, purge_bars: int, embargo_bars: int
) -> None:
    if n_samples < n_splits:
        pytest.skip("n_samples < n_splits is a documented ValueError case, tested separately")
    splits = purged_kfold_splits(n_samples, n_splits, purge_bars=purge_bars, embargo_bars=embargo_bars)
    assert len(splits) == n_splits
    _assert_purge_embargo_invariant(n_samples, splits, purge_bars=purge_bars, embargo_bars=embargo_bars)


def test_purged_kfold_folds_are_contiguous_and_cover_every_index_exactly_once() -> None:
    splits = purged_kfold_splits(23, 5)
    all_test = np.concatenate([s.test_indices for s in splits])
    assert sorted(all_test.tolist()) == list(range(23)), "every index must appear in exactly one test fold"
    assert len(all_test) == 23, "no index may repeat across test folds"
    for split in splits:
        diffs = np.diff(split.test_indices)
        assert np.all(diffs == 1), f"test fold {split.test_indices} is not contiguous"


def test_purged_kfold_no_purge_no_embargo_is_plain_kfold() -> None:
    # Hand-verified: n=20, k=4 -> four folds of exactly 5, train = everything else.
    splits = purged_kfold_splits(20, 4)
    assert [s.test_indices.tolist() for s in splits] == [
        [0, 1, 2, 3, 4],
        [5, 6, 7, 8, 9],
        [10, 11, 12, 13, 14],
        [15, 16, 17, 18, 19],
    ]
    assert splits[0].train_indices.tolist() == list(range(5, 20))
    assert splits[1].train_indices.tolist() == list(range(5)) + list(range(10, 20))


def test_purged_kfold_hand_verified_purge_and_embargo() -> None:
    # Hand-verified: n=20, k=4, purge=2, embargo=1 (see round-1 implementation notes).
    splits = purged_kfold_splits(20, 4, purge_bars=2, embargo_bars=1)
    assert splits[0].test_indices.tolist() == [0, 1, 2, 3, 4]
    assert splits[0].train_indices.tolist() == list(range(8, 20))
    assert splits[1].test_indices.tolist() == [5, 6, 7, 8, 9]
    assert splits[1].train_indices.tolist() == [0, 1, 2] + list(range(13, 20))


def test_purged_kfold_uneven_split_sizes_differ_by_at_most_one() -> None:
    splits = purged_kfold_splits(10, 3)  # 10/3 -> sizes [4, 3, 3]
    sizes = sorted(len(s.test_indices) for s in splits)
    assert sizes == [3, 3, 4]


def test_purged_kfold_rejects_too_few_splits() -> None:
    with pytest.raises(ValueError, match="n_splits"):
        purged_kfold_splits(10, 1)


def test_purged_kfold_rejects_more_splits_than_samples() -> None:
    with pytest.raises(ValueError, match="n_samples"):
        purged_kfold_splits(3, 5)


def test_purged_kfold_rejects_negative_purge_or_embargo() -> None:
    with pytest.raises(ValueError, match="purge_bars"):
        purged_kfold_splits(10, 2, purge_bars=-1)
    with pytest.raises(ValueError, match="embargo_bars"):
        purged_kfold_splits(10, 2, embargo_bars=-1)


def test_purged_kfold_large_purge_can_empty_a_train_set() -> None:
    """Not an error -- a caller who asks for more purge than the data can support gets an
    empty (or very small) train set for that fold, which the walk_forward orchestrator's
    own grid search will need to detect and refuse rather than silently proceed."""
    splits = purged_kfold_splits(10, 2, purge_bars=100, embargo_bars=100)
    assert len(splits[0].train_indices) == 0


# ---------------------------------------------------------------------------
# walk_forward_splits(): exhaustive invariant + hand-verified small cases
# ---------------------------------------------------------------------------


def _assert_walk_forward_invariant(splits: list[Split], *, purge_bars: int) -> None:
    for split in splits:
        train_set = set(split.train_indices.tolist())
        test_set = set(split.test_indices.tolist())
        assert train_set.isdisjoint(test_set)
        assert max(split.train_indices) < min(split.test_indices), "train must entirely precede test"
        gap = min(split.test_indices) - max(split.train_indices) - 1
        assert gap == purge_bars, f"expected exactly {purge_bars} purge bars between train and test, got {gap}"
        assert np.all(np.diff(split.train_indices) == 1), "train indices must be contiguous"
        assert np.all(np.diff(split.test_indices) == 1), "test indices must be contiguous"


@pytest.mark.parametrize("expanding", [False, True])
@pytest.mark.parametrize("purge_bars", [0, 1, 5])
@pytest.mark.parametrize(("train_window", "test_window", "step"), [(20, 10, None), (30, 5, 5), (15, 15, 7)])
def test_walk_forward_invariant_holds_across_parameter_matrix(
    expanding: bool, purge_bars: int, train_window: int, test_window: int, step: int | None
) -> None:
    splits = walk_forward_splits(
        200, train_window=train_window, test_window=test_window, step=step, purge_bars=purge_bars, expanding=expanding
    )
    assert len(splits) >= 1
    _assert_walk_forward_invariant(splits, purge_bars=purge_bars)
    if expanding:
        assert all(s.train_indices[0] == 0 for s in splits), "expanding mode's train window always starts at 0"
        train_lengths = [len(s.train_indices) for s in splits]
        assert train_lengths == sorted(train_lengths), "expanding mode's train window must never shrink"


def test_walk_forward_rolling_hand_verified() -> None:
    splits = walk_forward_splits(20, train_window=6, test_window=3, purge_bars=1)
    assert [s.train_indices.tolist() for s in splits] == [
        [0, 1, 2, 3, 4, 5],
        [3, 4, 5, 6, 7, 8],
        [6, 7, 8, 9, 10, 11],
        [9, 10, 11, 12, 13, 14],
    ]
    assert [s.test_indices.tolist() for s in splits] == [
        [7, 8, 9],
        [10, 11, 12],
        [13, 14, 15],
        [16, 17, 18],
    ]


def test_walk_forward_expanding_hand_verified() -> None:
    splits = walk_forward_splits(20, train_window=6, test_window=3, expanding=True)
    assert [len(s.train_indices) for s in splits] == [6, 9, 12, 15]
    assert splits[0].train_indices.tolist() == list(range(6))
    assert splits[-1].train_indices.tolist() == list(range(15))
    assert [s.test_indices.tolist() for s in splits] == [[6, 7, 8], [9, 10, 11], [12, 13, 14], [15, 16, 17]]


def test_walk_forward_rolling_train_window_never_grows() -> None:
    splits = walk_forward_splits(100, train_window=10, test_window=5, expanding=False)
    assert all(len(s.train_indices) == 10 for s in splits)


def test_walk_forward_trailing_partial_fold_dropped_not_shrunk() -> None:
    # n=23, train=10, test=5, step=5: folds at test_start=10,15,20 -- the last would need
    # [20,25) but only 3 rows remain, so it must be dropped entirely, not truncated to 3.
    splits = walk_forward_splits(23, train_window=10, test_window=5)
    assert len(splits) == 2
    assert splits[-1].test_indices.tolist() == [15, 16, 17, 18, 19]
    assert all(len(s.test_indices) == 5 for s in splits), "no fold may have a shrunk test window"


def test_walk_forward_no_room_for_even_one_fold_raises() -> None:
    with pytest.raises(ValueError, match="no room"):
        walk_forward_splits(5, train_window=6, test_window=3)


def test_walk_forward_rejects_non_positive_windows() -> None:
    with pytest.raises(ValueError, match="train_window"):
        walk_forward_splits(100, train_window=0, test_window=5)
    with pytest.raises(ValueError, match="test_window"):
        walk_forward_splits(100, train_window=10, test_window=0)


def test_walk_forward_rejects_negative_purge() -> None:
    with pytest.raises(ValueError, match="purge_bars"):
        walk_forward_splits(100, train_window=10, test_window=5, purge_bars=-1)


def test_walk_forward_rejects_non_positive_step() -> None:
    with pytest.raises(ValueError, match="step"):
        walk_forward_splits(100, train_window=10, test_window=5, step=0)


def test_walk_forward_default_step_equals_test_window_non_overlapping() -> None:
    splits = walk_forward_splits(100, train_window=10, test_window=5)
    test_starts = [int(s.test_indices[0]) for s in splits]
    assert test_starts == sorted(set(test_starts)), "default step must produce non-overlapping test folds"
    for a, b in pairwise(splits):
        assert int(b.test_indices[0]) - int(a.test_indices[0]) == 5
