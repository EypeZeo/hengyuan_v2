"""Purged K-Fold + embargo, and walk-forward, time-series index splitting — 批次 2.

Both functions here produce **integer positional indices** into an aligned OHLCV series
(the same row order ``records_to_dataframe()``/``run_vectorized_backtest()`` already use),
not timestamps or dates -- callers slice their own records/signal lists with them.

**Why purge and embargo at all, given this repo's strategies today are simple
next-bar-open rule-based signals with no explicit forward-looking label window?** Standard
K-Fold assumes each sample's "evaluation" is self-contained; it is not, the moment a
sample's outcome depends on data outside its own row -- which includes not just an explicit
ML label horizon (batch 7's eventual concern) but also a rolling-window indicator's warm-up
tail and the very real serial correlation of financial returns across adjacent bars. Purging
removes train rows whose stated ``purge_bars`` window could overlap a test fold's
information; embargo additionally removes a margin of rows immediately after a test fold,
guarding specifically against serial-correlation leakage back into training. Both default to
0 (a plain, unleaking K-Fold) so today's zero-lookahead-horizon strategies pay no unnecessary
cost, and batch 7's future label-based work has the machinery already in place.

**The one invariant every split from both functions must satisfy, and that the test suite
checks exhaustively rather than by example:** no train index falls within ``purge_bars`` of
any test index on either side, and no train index falls within ``embargo_bars`` immediately
after the post-purge boundary of any test fold. Folds are always contiguous, chronologically
ordered blocks -- **never shuffled** -- shuffling a time series before splitting is exactly
the kind of lookahead this package exists to prevent, not a detail to get right later.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np


@dataclass(frozen=True, slots=True)
class Split:
    """One train/test partition. Both arrays are sorted ascending int64 positional indices,
    disjoint, and (for a well-formed split) leave a purge_bars/embargo_bars gap between
    them -- see module docstring."""

    train_indices: np.ndarray
    test_indices: np.ndarray


def _contiguous_fold_boundaries(n_samples: int, n_splits: int) -> list[tuple[int, int]]:
    """n_splits contiguous, chronologically ordered blocks covering [0, n_samples), sizes
    differing by at most 1 (the standard "distribute the remainder across the first folds"
    rule -- e.g. 10 samples / 3 splits -> block sizes [4, 3, 3])."""
    if n_splits < 2:
        raise ValueError(f"n_splits must be >= 2, got {n_splits}")
    if n_samples < n_splits:
        raise ValueError(f"n_samples ({n_samples}) must be >= n_splits ({n_splits})")
    base_size, remainder = divmod(n_samples, n_splits)
    boundaries: list[tuple[int, int]] = []
    start = 0
    for i in range(n_splits):
        size = base_size + (1 if i < remainder else 0)
        boundaries.append((start, start + size))
        start += size
    return boundaries


def purged_kfold_splits(
    n_samples: int,
    n_splits: int,
    *,
    purge_bars: int = 0,
    embargo_bars: int = 0,
) -> list[Split]:
    """n_splits contiguous folds; each fold in turn is the test set, all other rows are the
    train set minus the purge/embargo exclusions (see module docstring for exactly what
    those remove and why).

    For test fold [test_start, test_end):
    - purge train rows in [test_start - purge_bars, test_start) -- a train row here could
      have a purge-window outcome overlapping into the test fold's start.
    - purge train rows in [test_end, test_end + purge_bars) -- symmetric: a test row near
      the fold's end could have a purge-window outcome overlapping into the following block.
    - embargo train rows in [test_end + purge_bars, test_end + purge_bars + embargo_bars) --
      an additional margin beyond the post-test purge boundary, independent of any explicit
      window overlap, guarding against serial-correlation leakage.

    Raises:
        ValueError: n_splits < 2, n_samples < n_splits, or purge_bars/embargo_bars < 0.
    """
    if purge_bars < 0:
        raise ValueError(f"purge_bars must be >= 0, got {purge_bars}")
    if embargo_bars < 0:
        raise ValueError(f"embargo_bars must be >= 0, got {embargo_bars}")

    boundaries = _contiguous_fold_boundaries(n_samples, n_splits)
    all_indices = np.arange(n_samples, dtype=np.int64)

    splits: list[Split] = []
    for test_start, test_end in boundaries:
        excluded = np.zeros(n_samples, dtype=bool)
        excluded[test_start:test_end] = True

        pre_purge_start = max(0, test_start - purge_bars)
        excluded[pre_purge_start:test_start] = True

        post_purge_end = min(n_samples, test_end + purge_bars)
        excluded[test_end:post_purge_end] = True

        embargo_end = min(n_samples, post_purge_end + embargo_bars)
        excluded[post_purge_end:embargo_end] = True

        train_indices = all_indices[~excluded]
        test_indices = all_indices[test_start:test_end]
        splits.append(Split(train_indices=train_indices, test_indices=test_indices))

    return splits


def walk_forward_splits(
    n_samples: int,
    *,
    train_window: int,
    test_window: int,
    step: int | None = None,
    purge_bars: int = 0,
    expanding: bool = False,
) -> list[Split]:
    """Sequential, chronologically ordered folds: train on a window immediately before test,
    walk forward by ``step`` (default ``test_window`` -- non-overlapping test folds, the
    standard setup) until the data runs out.

    train_window: size of each fold's train window. Ignored (train always starts at index 0)
        when expanding=True -- see below.
    expanding: False (default, "rolling") -- every fold's train window is exactly
        train_window rows, sliding forward each step; this is the more realistic choice for
        "how would a live system that only keeps a bounded lookback actually behave".
        True ("expanding") -- every fold's train window starts at index 0 and grows to
        include everything before the test fold; more common in academic walk-forward
        writeups, uses strictly more information per fold than a live system with a fixed
        retraining lookback would have.
    purge_bars: gap between the end of train and the start of test, for the same reason
        purged_kfold_splits() has one (see module docstring) -- walk-forward's train-then-
        test-immediately-after layout has exactly the same "train row's window could
        overlap test" boundary concern as purged K-Fold's pre-test purge, just on one side
        only (there is no "after test" train block in walk-forward to purge symmetrically).
        No separate embargo parameter: purge_bars already sits at the one boundary that
        exists here (embargo's distinct role in purged_kfold_splits() is guarding the
        post-test train block specifically, which walk-forward doesn't have).

    Folds stop being generated once there is no room left for a full test_window; a
    trailing partial window is dropped rather than silently shrunk (a silently-shrunk final
    fold would make cross-fold OOS metric comparisons subtly apples-to-oranges).

    Raises:
        ValueError: train_window/test_window <= 0, purge_bars < 0, step <= 0, or the
            parameters leave no room for even one fold.
    """
    if train_window <= 0:
        raise ValueError(f"train_window must be > 0, got {train_window}")
    if test_window <= 0:
        raise ValueError(f"test_window must be > 0, got {test_window}")
    if purge_bars < 0:
        raise ValueError(f"purge_bars must be >= 0, got {purge_bars}")
    effective_step = step if step is not None else test_window
    if effective_step <= 0:
        raise ValueError(f"step must be > 0, got {step}")

    all_indices = np.arange(n_samples, dtype=np.int64)
    splits: list[Split] = []
    fold_index = 0
    while True:
        if expanding:
            train_start = 0
            train_end = train_window + fold_index * effective_step
        else:
            train_start = fold_index * effective_step
            train_end = train_start + train_window

        test_start = train_end + purge_bars
        test_end = test_start + test_window
        if test_end > n_samples:
            break

        train_indices = all_indices[train_start:train_end]
        test_indices = all_indices[test_start:test_end]
        splits.append(Split(train_indices=train_indices, test_indices=test_indices))
        fold_index += 1

    if not splits:
        raise ValueError(
            f"no room for even one fold: n_samples={n_samples}, train_window={train_window}, "
            f"test_window={test_window}, purge_bars={purge_bars}, step={effective_step}"
        )
    return splits
