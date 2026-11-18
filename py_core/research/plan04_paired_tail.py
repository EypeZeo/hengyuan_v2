"""Simultaneous stationary-bootstrap bands for fixed paired tail return differences."""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass
from decimal import Decimal
from numbers import Real

import numpy as np

from py_core.research.paired_bootstrap import stationary_bootstrap_indices
from py_core.research.plan04_risk import worst_five_percent_count


@dataclass(frozen=True, slots=True)
class TailBand:
    reference_tail: float
    trend_tail: float
    difference: float
    lower: float
    upper: float


@dataclass(frozen=True, slots=True)
class TailFamilyBands:
    half_width: float
    tail_observations: int
    bands: tuple[TailBand, ...]


def _return_matrix(returns: Sequence[Sequence[float]] | np.ndarray) -> np.ndarray:
    try:
        if np.ma.is_masked(returns):
            raise ValueError("masked returns are unsupported")
        raw = np.asarray(returns)
        if raw.dtype.kind not in "fiOu" or (
            raw.dtype.kind == "O" and any(
                isinstance(value, (bool, np.bool_)) or not isinstance(value, (Real, Decimal))
                for value in raw.flat
            )
        ):
            raise ValueError("returns must be real numbers")
        with np.errstate(over="ignore", invalid="ignore"):
            values = np.asarray(raw, dtype=np.float64)
    except (TypeError, ValueError, OverflowError) as error:
        raise ValueError("returns must form a complete real-valued matrix") from error
    if values.ndim != 2 or values.shape[0] < 2 or values.shape[1] < 2 or values.shape[1] % 2:
        raise ValueError("returns must have at least two dates and alternating reference and trend columns")
    if not np.all(np.isfinite(values)):
        raise ValueError("returns contain NaN or infinity")
    return values


def _tail_means(values: np.ndarray, count: int) -> np.ndarray:
    lowest = np.partition(values, count - 1, axis=-2)[..., :count, :]
    return np.mean(lowest, axis=-2)


def _ranked_tail_sums(counts: np.ndarray, ranked_values: np.ndarray, tail_count: int) -> tuple[np.ndarray, np.ndarray]:
    cumulative = np.cumsum(counts, axis=1)
    missing = cumulative[:, -1] < tail_count
    np.minimum(cumulative, tail_count, out=cumulative)
    selected = np.empty_like(cumulative)
    selected[:, 0] = cumulative[:, 0]
    np.subtract(cumulative[:, 1:], cumulative[:, :-1], out=selected[:, 1:])
    sums = np.einsum("ij,j->i", selected, ranked_values)
    return sums, missing


def _resampled_tail_means(values: np.ndarray, indices: np.ndarray, tail_count: int) -> np.ndarray:
    """Count each day's multiplicity and select the same lowest returns as a sorted draw."""
    days, columns = values.shape
    rows = np.arange(len(indices), dtype=np.intp)[:, None] * days
    counts = np.bincount((rows + indices).ravel(), minlength=len(indices) * days).reshape(-1, days)
    order = np.argsort(values, axis=0)
    prefix = min(days, max(32, 3 * tail_count))
    means = np.empty((len(indices), columns))
    for column in range(columns):
        ranks = order[:, column]
        first = ranks[:prefix]
        sums, missing = _ranked_tail_sums(counts[:, first], values[first, column], tail_count)
        if np.any(missing):
            sums[missing], _ = _ranked_tail_sums(
                counts[missing][:, ranks], values[ranks, column], tail_count
            )
        means[:, column] = sums / tail_count
    return means


def _bands_from_half_width(
    tails: np.ndarray, observed: np.ndarray, half_width: float, count: int
) -> TailFamilyBands:
    with np.errstate(over="ignore", invalid="ignore"):
        lower, upper = observed - half_width, observed + half_width
    if not np.all(np.isfinite(lower)) or not np.all(np.isfinite(upper)):
        raise ValueError("bootstrap produced a non-finite tail band")
    bands = tuple(
        TailBand(float(ref), float(trend), float(diff), float(lo), float(hi))
        for ref, trend, diff, lo, hi in zip(tails[::2], tails[1::2], observed, lower, upper, strict=True)
    )
    return TailFamilyBands(half_width, count, bands)


def bootstrap_tail_band_levels(
    returns: Sequence[Sequence[float]] | np.ndarray,
    *,
    block_length: float,
    n_resamples: int,
    rng: np.random.Generator,
    levels_per_thousand: tuple[int, ...],
) -> tuple[TailFamilyBands, ...]:
    """Return one band per rank level, in input order, from shared resampled errors."""
    if not levels_per_thousand or any(
        type(level) is not int or not 0 < level < 1000 for level in levels_per_thousand
    ):
        raise ValueError("rank levels must be integers between 1 and 999")
    values = _return_matrix(returns)
    count = worst_five_percent_count(values.shape[0])
    indices = stationary_bootstrap_indices(
        values.shape[0], block_length=block_length, n_resamples=n_resamples, rng=rng
    )
    draws = len(indices)
    ranks = tuple((level * (draws + 1) + 999) // 1000 for level in levels_per_thousand)
    if max(ranks) > draws:
        raise ValueError("too few resamples for the requested simultaneous tail band")
    with np.errstate(over="ignore", invalid="ignore"):
        tails = _tail_means(values, count)
        observed = tails[1::2] - tails[::2]
        resampled = _resampled_tail_means(values, indices, count)
        differences = resampled[:, 1::2] - resampled[:, ::2]
        max_errors = np.max(np.abs(differences - observed), axis=1)
        if not np.all(np.isfinite(max_errors)):
            raise ValueError("bootstrap produced a non-finite tail error")

    positions = tuple(rank - 1 for rank in ranks)
    ordered_at_ranks = np.partition(max_errors, positions)
    return tuple(
        _bands_from_half_width(tails, observed, float(ordered_at_ranks[index]), count)
        for index in positions
    )


def bootstrap_tail_bands(
    returns: Sequence[Sequence[float]] | np.ndarray,
    *,
    block_length: float,
    n_resamples: int,
    rng: np.random.Generator,
) -> TailFamilyBands:
    """Estimate paired tail differences using shared date draws and one family half-width."""
    return bootstrap_tail_band_levels(
        returns,
        block_length=block_length,
        n_resamples=n_resamples,
        rng=rng,
        levels_per_thousand=(950,),
    )[0]


def bootstrap_tail_band_variants(
    returns: Sequence[Sequence[float]] | np.ndarray,
    *,
    block_length: float,
    n_resamples: int,
    rng: np.random.Generator,
) -> tuple[TailFamilyBands, TailFamilyBands]:
    """Return the original and prespecified wider bands using the same resampled errors."""
    baseline, adjusted = bootstrap_tail_band_levels(
        returns,
        block_length=block_length,
        n_resamples=n_resamples,
        rng=rng,
        levels_per_thousand=(950, 965),
    )
    return baseline, adjusted


def classify_tail_direction(bands: Sequence[TailBand]) -> str:
    """Report a direction only when every specified block length agrees strictly."""
    if not bands:
        raise ValueError("at least one block-length result is required")
    if all(band.upper < 0 for band in bands):
        return "stable_lower"
    if all(band.lower > 0 for band in bands):
        return "stable_higher"
    return "inconclusive"
