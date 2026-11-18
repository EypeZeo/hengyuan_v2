"""Tests for paired stationary-bootstrap bands on lower-tail returns."""

from __future__ import annotations

import math

import numpy as np
import pytest

from py_core.research import plan04_paired_tail
from py_core.research.plan04_paired_tail import (
    TailBand,
    bootstrap_tail_bands,
    classify_tail_direction,
)


def test_identical_reference_and_trend_columns_have_zero_width_and_inconclusive_direction() -> None:
    reference = np.linspace(-0.04, 0.03, 20)
    returns = np.column_stack((reference, reference, reference * 2, reference * 2))

    result = bootstrap_tail_bands(
        returns,
        block_length=3,
        n_resamples=39,
        rng=np.random.default_rng(101),
    )

    assert result.tail_observations == 1
    assert result.half_width == 0
    assert len(result.bands) == 2
    for band in result.bands:
        assert band.reference_tail == band.trend_tail
        assert band.difference == 0
        assert band.lower == band.upper == 0
        assert classify_tail_direction((band,)) == "inconclusive"


def test_constant_paired_shift_has_known_tail_difference_and_direction() -> None:
    reference = np.linspace(-0.02, 0.04, 20)
    shift = 0.025
    result = bootstrap_tail_bands(
        np.column_stack((reference, reference + shift)),
        block_length=2,
        n_resamples=39,
        rng=np.random.default_rng(202),
    )

    band = result.bands[0]
    assert band.reference_tail == pytest.approx(float(np.min(reference)))
    assert band.trend_tail == pytest.approx(float(np.min(reference)) + shift)
    assert band.difference == pytest.approx(shift)
    assert result.half_width == pytest.approx(0, abs=1e-12)
    assert band.lower == pytest.approx(shift)
    assert band.upper == pytest.approx(shift)
    assert classify_tail_direction((band,)) == "stable_higher"


def test_simultaneous_half_width_matches_shared_index_family_max_oracle(monkeypatch: pytest.MonkeyPatch) -> None:
    n_dates = 20
    n_resamples = 39
    date = np.arange(n_dates, dtype=np.float64)
    returns = np.column_stack(
        (
            np.sin(date) / 10,
            np.cos(date) / 10,
            (date - 8) / 100,
            np.sin(date * 0.7) / 8,
        )
    )
    index_draws = np.random.default_rng(404).integers(0, n_dates, size=(n_resamples, n_dates))
    calls: list[tuple[int, float, int, np.random.Generator]] = []

    def fixed_indices(
        observations: int,
        *,
        block_length: float,
        n_resamples: int,
        rng: np.random.Generator,
    ) -> np.ndarray:
        calls.append((observations, block_length, n_resamples, rng))
        return index_draws.copy()

    monkeypatch.setattr(plan04_paired_tail, "stationary_bootstrap_indices", fixed_indices)

    result_rng = np.random.default_rng(405)
    result = bootstrap_tail_bands(
        returns,
        block_length=4,
        n_resamples=n_resamples,
        rng=result_rng,
    )

    tail_count = math.ceil(n_dates * 0.05)
    observed_tails = np.sort(returns, axis=0)[:tail_count].mean(axis=0)
    observed_differences = observed_tails[1::2] - observed_tails[::2]
    errors = np.empty((n_resamples, returns.shape[1] // 2))
    for index, selected_rows in enumerate(index_draws):
        draw_tails = np.sort(returns[selected_rows], axis=0)[:tail_count].mean(axis=0)
        draw_differences = draw_tails[1::2] - draw_tails[::2]
        errors[index] = draw_differences - observed_differences
    family_errors = np.max(np.abs(errors), axis=1)
    rank = math.ceil((n_resamples + 1) * 0.95)
    expected_half_width = np.sort(family_errors)[rank - 1]

    assert len(calls) == 1
    assert calls[0][:3] == (n_dates, 4, n_resamples)
    assert calls[0][3] is result_rng
    assert result.tail_observations == tail_count
    assert result.half_width == pytest.approx(expected_half_width)
    for index, band in enumerate(result.bands):
        expected_difference = observed_differences[index]
        assert band.reference_tail == pytest.approx(observed_tails[2 * index])
        assert band.trend_tail == pytest.approx(observed_tails[2 * index + 1])
        assert band.difference == pytest.approx(expected_difference)
        assert band.lower == pytest.approx(expected_difference - expected_half_width)
        assert band.upper == pytest.approx(expected_difference + expected_half_width)


@pytest.mark.parametrize(
    "bands, expected",
    [
        ((TailBand(0, 0, -0.3, -0.5, -0.2), TailBand(0, 0, -0.1, -0.2, -0.05)), "stable_lower"),
        ((TailBand(0, 0, 0.3, 0.2, 0.5), TailBand(0, 0, 0.1, 0.05, 0.2)), "stable_higher"),
        ((TailBand(0, 0, -0.1, -0.2, 0),), "inconclusive"),
        ((TailBand(0, 0, 0.1, 0, 0.2),), "inconclusive"),
        ((TailBand(0, 0, 0, 0, 0),), "inconclusive"),
        ((TailBand(0, 0, -0.1, -0.2, -0.05), TailBand(0, 0, 0.1, 0.05, 0.2)), "inconclusive"),
    ],
)
def test_direction_requires_strict_agreement_across_block_lengths(
    bands: tuple[TailBand, ...], expected: str
) -> None:
    assert classify_tail_direction(bands) == expected


def test_direction_rejects_an_empty_band_sequence() -> None:
    with pytest.raises(ValueError):
        classify_tail_direction(())


@pytest.mark.parametrize(
    "returns",
    [
        None,
        [0.0, 1.0],
        [[0.0, 1.0]],
        np.zeros((2, 0)),
        np.zeros((0, 2)),
        np.array([[0.0, 1.0, 2.0], [1.0, 2.0, 3.0]]),
        np.array([[0.0, float("nan")], [1.0, 2.0]]),
        np.array([[0.0, float("inf")], [1.0, 2.0]]),
        np.array([[0.0 + 1.0j, 1.0], [1.0 + 2.0j, 2.0]], dtype=np.complex128),
        np.ma.array([[0.0, 1.0], [1.0, 2.0]], mask=[[False, True], [False, False]]),
        np.array([[False, True], [True, False]], dtype=bool),
    ],
    ids=["missing", "one-dimensional", "one-date", "empty-columns", "empty", "odd-family", "nan", "infinity", "complex", "masked", "boolean"],
)
def test_bootstrap_rejects_nonfinite_or_malformed_return_matrices(returns: object) -> None:
    with pytest.raises(ValueError):
        bootstrap_tail_bands(
            returns,  # type: ignore[arg-type]
            block_length=2,
            n_resamples=39,
            rng=np.random.default_rng(909),
        )


@pytest.mark.parametrize(
    ("block_length", "n_resamples"),
    [(0, 39), (2, 18), (2, 1)],
)
def test_bootstrap_rejects_invalid_block_length_or_too_few_resamples(
    block_length: int, n_resamples: int
) -> None:
    with pytest.raises(ValueError):
        bootstrap_tail_bands(
            np.zeros((20, 2)),
            block_length=block_length,
            n_resamples=n_resamples,
            rng=np.random.default_rng(21),
        )
