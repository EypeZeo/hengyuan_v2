"""Combinatorial Purged Cross-Validation (CPCV) — 批次 2, round 2. Index math only; see
``cpcv_analysis.py`` for the orchestrator that actually trains/evaluates a strategy against
these splits.

Lopez de Prado's CPCV (*Advances in Financial Machine Learning*, ch 12): partition the data
into ``n_groups`` contiguous, chronologically ordered blocks (same construction
``purged_kfold_splits()`` uses -- see ``py_core.validation.splits``). Every way of choosing
``n_test_groups`` of those blocks as a combination's test set is enumerated
(``C(n_groups, n_test_groups)`` combinations total); each combination's train set is every
OTHER block, purged/embargoed around each chosen test block's boundary exactly like
``purged_kfold_splits()`` does for a single test fold, generalized here to (possibly
several, possibly adjacent) simultaneously-excluded test blocks.

**Why this exists on top of ``walk_forward_splits()``: CPCV produces a *distribution* of
out-of-sample performance, not one number.** A single walk-forward run gives one OOS
Sharpe; a strategy/parameter-search process that happened to get lucky on that one
particular historical path is indistinguishable from a genuinely robust one by that single
number alone -- exactly the batch-2 package docstring's founding question. CPCV instead
reconstructs many different complete OOS paths through the SAME data (by recombining test
results from different combinations -- see ``cpcv_paths()``) and hands the resulting Sharpe
*distribution* to round 3's PBO/Deflated-Sharpe machinery, which is the whole reason this
exists as its own component before those are built.

**Path reconstruction (``cpcv_paths()``): the elegant part of CPCV.** Every one of the
``n_groups`` groups appears as a test block in exactly ``C(n_groups - 1, n_test_groups - 1)``
combinations (call this ``phi``) -- a symmetric combinatorial fact, independent of which
group. A "path" is one way of picking, for every group, ONE of the ``phi`` combinations
that tested it, such that across all ``phi`` paths, every ``(combination, group)`` test
result gets used in exactly one path -- no reuse, no waste (``phi * n_groups`` combinatorial
"slots" exactly equals ``n_test_groups * C(n_groups, n_test_groups)`` "supply", the total
test-group results produced across all combinations, by the identity
``phi = n_test_groups * C(n_groups, n_test_groups) / n_groups``). This holds with a
construction that needs NO cross-group coordination at all: for each group independently,
list the combinations that test it (canonical order) and assign the p-th one to path p.
Verified both by direct derivation and, in the test suite, by an exhaustive covering-property
check across a parameter matrix of ``(n_groups, n_test_groups)`` plus a fully hand-enumerated
``n_groups=6, n_test_groups=2`` example (15 combinations, 5 paths).

A path's groups are generally each supplied by a DIFFERENT combination -- that is expected
and correct, not a bug: a CPCV path is a recombination across combinations, never a single
combination's own train/test split played back directly (that's already what plain
``purged_kfold_splits()`` is).
"""

from __future__ import annotations

import itertools
import math
from dataclasses import dataclass

import numpy as np

from py_core.validation.splits import _contiguous_fold_boundaries


@dataclass(frozen=True, slots=True)
class CpcvCombination:
    """One combination: ``n_test_groups`` blocks held out as test, everything else (minus
    purge/embargo around each held-out block's boundary) is train."""

    combination_index: int
    test_groups: tuple[int, ...]
    train_indices: np.ndarray
    test_indices_by_group: dict[int, np.ndarray]


@dataclass(frozen=True, slots=True)
class CpcvPath:
    """One reconstructed full-length OOS path: ``combination_for_group[g]`` is the index
    (into ``cpcv_combinations()``'s output list, for the SAME ``n_groups``/``n_test_groups``)
    supplying group ``g``'s test result for this path."""

    path_index: int
    combination_for_group: tuple[int, ...]


def _all_test_group_combinations(n_groups: int, n_test_groups: int) -> list[tuple[int, ...]]:
    """Shared canonical enumeration -- both ``cpcv_combinations()`` and ``cpcv_paths()`` call
    this so a combination index means the same thing in both functions' output, by
    construction rather than by convention."""
    return list(itertools.combinations(range(n_groups), n_test_groups))


def _validate_group_params(n_groups: int, n_test_groups: int) -> None:
    if n_groups < 2:
        raise ValueError(f"n_groups must be >= 2, got {n_groups}")
    if n_test_groups < 1:
        raise ValueError(f"n_test_groups must be >= 1, got {n_test_groups}")
    if n_test_groups >= n_groups:
        raise ValueError(
            f"n_test_groups ({n_test_groups}) must be < n_groups ({n_groups}) -- at least one "
            "group must remain available for train"
        )


def cpcv_combinations(
    n_samples: int,
    n_groups: int,
    n_test_groups: int,
    *,
    purge_bars: int = 0,
    embargo_bars: int = 0,
) -> list[CpcvCombination]:
    """All ``C(n_groups, n_test_groups)`` combinations of test blocks, each with its own
    purged/embargoed train set. See module docstring.

    Raises:
        ValueError: n_groups < 2, n_test_groups not in [1, n_groups - 1], n_samples <
            n_groups, or purge_bars/embargo_bars < 0.
    """
    _validate_group_params(n_groups, n_test_groups)
    if purge_bars < 0:
        raise ValueError(f"purge_bars must be >= 0, got {purge_bars}")
    if embargo_bars < 0:
        raise ValueError(f"embargo_bars must be >= 0, got {embargo_bars}")

    boundaries = _contiguous_fold_boundaries(n_samples, n_groups)
    all_indices = np.arange(n_samples, dtype=np.int64)
    combos = _all_test_group_combinations(n_groups, n_test_groups)

    result: list[CpcvCombination] = []
    for combination_index, test_groups in enumerate(combos):
        excluded = np.zeros(n_samples, dtype=bool)
        test_indices_by_group: dict[int, np.ndarray] = {}
        for g in test_groups:
            start, end = boundaries[g]
            test_indices_by_group[g] = all_indices[start:end]
            excluded[start:end] = True

            pre_purge_start = max(0, start - purge_bars)
            excluded[pre_purge_start:start] = True

            post_purge_end = min(n_samples, end + purge_bars)
            excluded[end:post_purge_end] = True

            embargo_end = min(n_samples, post_purge_end + embargo_bars)
            excluded[post_purge_end:embargo_end] = True

        train_indices = all_indices[~excluded]
        result.append(
            CpcvCombination(
                combination_index=combination_index,
                test_groups=test_groups,
                train_indices=train_indices,
                test_indices_by_group=test_indices_by_group,
            )
        )
    return result


def cpcv_paths(n_groups: int, n_test_groups: int) -> list[CpcvPath]:
    """``phi = C(n_groups - 1, n_test_groups - 1)`` reconstructed paths. Pure combinatorics
    over group indices -- does not need ``n_samples`` or purge/embargo (those only affect
    ``cpcv_combinations()``'s row-level math, not which combination supplies which group to
    which path).

    Raises:
        ValueError: n_groups < 2 or n_test_groups not in [1, n_groups - 1].
    """
    _validate_group_params(n_groups, n_test_groups)

    combos = _all_test_group_combinations(n_groups, n_test_groups)
    combo_indices_by_group: list[list[int]] = [
        [ci for ci, combo in enumerate(combos) if g in combo] for g in range(n_groups)
    ]
    phi = math.comb(n_groups - 1, n_test_groups - 1)

    paths: list[CpcvPath] = []
    for p in range(phi):
        combination_for_group = tuple(combo_indices_by_group[g][p] for g in range(n_groups))
        paths.append(CpcvPath(path_index=p, combination_for_group=combination_for_group))
    return paths
