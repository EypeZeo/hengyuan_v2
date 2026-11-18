"""Simultaneous stationary-bootstrap bands for fixed paired tail return differences."""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass
from decimal import Decimal
from numbers import Real

import numpy as np

from py_core.research.paired_bootstrap import stationary_bootstrap_indices
from py_core.research.plan04_risk import worst_five_percent_count

_BATCH_SIZE = 128


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


def bootstrap_tail_bands(
    returns: Sequence[Sequence[float]] | np.ndarray,
    *,
    block_length: float,
    n_resamples: int,
    rng: np.random.Generator,
) -> TailFamilyBands:
    """Estimate paired tail differences using shared date draws and one family half-width."""
    values = _return_matrix(returns)
    count = worst_five_percent_count(values.shape[0])
    indices = stationary_bootstrap_indices(
        values.shape[0], block_length=block_length, n_resamples=n_resamples, rng=rng
    )
    draws = len(indices)
    rank = (95 * (draws + 1) + 99) // 100
    if rank > draws:
        raise ValueError("too few resamples for the simultaneous 95% tail band")
    max_errors = np.empty(draws)
    with np.errstate(over="ignore", invalid="ignore"):
        tails = _tail_means(values, count)
        observed = tails[1::2] - tails[::2]
        for start in range(0, draws, _BATCH_SIZE):
            block = slice(start, start + _BATCH_SIZE)
            resampled = _tail_means(values[indices[block]], count)
            differences = resampled[:, 1::2] - resampled[:, ::2]
            max_errors[block] = np.max(np.abs(differences - observed), axis=1)
        if not np.all(np.isfinite(max_errors)):
            raise ValueError("bootstrap produced a non-finite tail error")

        half_width = float(np.partition(max_errors, rank - 1)[rank - 1])
        lower, upper = observed - half_width, observed + half_width
        if not np.all(np.isfinite(lower)) or not np.all(np.isfinite(upper)):
            raise ValueError("bootstrap produced a non-finite tail band")

    bands = tuple(
        TailBand(float(ref), float(trend), float(diff), float(lo), float(hi))
        for ref, trend, diff, lo, hi in zip(tails[::2], tails[1::2], observed, lower, upper, strict=True)
    )
    return TailFamilyBands(half_width, count, bands)


def classify_tail_direction(bands: Sequence[TailBand]) -> str:
    """Report a direction only when every specified block length agrees strictly."""
    if not bands:
        raise ValueError("at least one block-length result is required")
    if all(band.upper < 0 for band in bands):
        return "stable_lower"
    if all(band.lower > 0 for band in bands):
        return "stable_higher"
    return "inconclusive"
