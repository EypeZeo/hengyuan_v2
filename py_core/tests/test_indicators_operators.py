"""Tests for py_core.indicators.operators (批次 3, round 1).

Every operator here has a hand-computed exact-value check (not just shape/monotonicity
assertions) since these functions exist specifically to pin down a *unique* computation
method (docs/STRATEGY_SPEC.md §2.2) a future C++ evaluator must reproduce bit-for-bit --
"looks about right" isn't the bar here. The NaN-prefix composition tests are the ones that
matter most architecturally: they prove the invariant operators.py's module docstring
documents (ema()/rsi() correctly skip an upstream operator's own leading NaN run rather than
assuming input starts at position 0) actually holds, which is what makes §6's additive
warm-up formula well-defined across an arbitrarily deep DAG.
"""

from __future__ import annotations

import numpy as np
import pandas as pd
import pytest

from py_core.indicators import operators as op


def _series(values: list[float]) -> pd.Series:
    return pd.Series(values, dtype=np.float64, index=pd.RangeIndex(len(values)))


# ---------------------------------------------------------------------------
# sma
# ---------------------------------------------------------------------------


def test_sma_hand_verified() -> None:
    x = _series([1, 2, 3, 4, 5, 6, 7, 8, 9, 10])
    result = op.sma(x, 3)
    assert result.iloc[:2].isna().all()
    assert result.iloc[2] == pytest.approx(2.0)
    assert result.iloc[3] == pytest.approx(3.0)
    assert result.iloc[9] == pytest.approx(9.0)


def test_sma_rejects_non_positive_window() -> None:
    with pytest.raises(ValueError, match="window"):
        op.sma(_series([1.0, 2.0]), 0)


def test_sma_composes_correctly_on_top_of_nan_prefixed_input() -> None:
    x = _series([1, 2, 3, 4, 5, 6, 7, 8, 9, 10])
    inner = op.sma(x, 3)  # NaN at positions 0, 1
    outer = op.sma(inner, 2)
    # inner valid from position 2; outer needs 2 valid inner values -> valid from position 3
    assert outer.iloc[:3].isna().all()
    assert outer.iloc[3] == pytest.approx((inner.iloc[2] + inner.iloc[3]) / 2.0)


# ---------------------------------------------------------------------------
# ema
# ---------------------------------------------------------------------------


def test_ema_hand_verified_sma_seeded() -> None:
    x = _series([1, 2, 3, 4, 5, 6, 7, 8, 9, 10])
    result = op.ema(x, 3)  # alpha = 2/4 = 0.5
    assert result.iloc[:2].isna().all()
    assert result.iloc[2] == pytest.approx(2.0)  # SMA(1,2,3), not x[0]
    assert result.iloc[3] == pytest.approx(0.5 * 4 + 0.5 * 2.0)
    assert result.iloc[4] == pytest.approx(0.5 * 5 + 0.5 * result.iloc[3])


def test_ema_rejects_non_positive_window() -> None:
    with pytest.raises(ValueError, match="window"):
        op.ema(_series([1.0, 2.0]), 0)


def test_ema_composes_correctly_on_top_of_nan_prefixed_input() -> None:
    x = _series([1, 2, 3, 4, 5, 6, 7, 8, 9, 10])
    inner = op.sma(x, 3)  # NaN at positions 0, 1, valid from position 2
    outer = op.ema(inner, 2)  # alpha = 2/3
    assert outer.iloc[:3].isna().all()
    seed = (inner.iloc[2] + inner.iloc[3]) / 2.0
    assert outer.iloc[3] == pytest.approx(seed)
    alpha = 2.0 / 3.0
    assert outer.iloc[4] == pytest.approx(alpha * inner.iloc[4] + (1 - alpha) * seed)


def test_ema_all_nan_input_stays_all_nan() -> None:
    x = pd.Series([np.nan] * 5, index=pd.RangeIndex(5))
    result = op.ema(x, 3)
    assert result.isna().all()


# Audit F-A-009. A NaN in the MIDDLE of a stream (a ``div`` by zero upstream is enough) used to make ema() recurse through its own previous
# NaN and stay NaN for ever, while the native step_ema kept its state and carried on: the two sides then disagreed on every bar after the
# gap. Both now behave the way the native side always did, and native/tests/test_strategy_spec_operators.cpp pins the SAME sequences.


def test_ema_mid_stream_nan_costs_that_bar_only() -> None:
    result = op.ema(_series([1.0, 2.0, np.nan, 4.0, 5.0]), 2)  # alpha = 2/3
    assert np.isnan(result.iloc[0]) and np.isnan(result.iloc[2])
    assert result.iloc[1] == pytest.approx(1.5)
    assert result.iloc[3] == pytest.approx(2.0 / 3.0 * 4.0 + 1.0 / 3.0 * 1.5)  # 3.1666666667: the recursion resumed from 1.5
    assert result.iloc[4] == pytest.approx(2.0 / 3.0 * 5.0 + 1.0 / 3.0 * result.iloc[3])  # 4.3888888889


def test_ema_nan_before_the_seed_is_complete_is_not_counted_in_the_seed() -> None:
    result = op.ema(_series([1.0, np.nan, 3.0, 5.0]), 2)
    assert result.iloc[:2].isna().all()
    assert result.iloc[2] == pytest.approx(2.0)  # mean of the first two NON-NaN values, 1 and 3
    assert result.iloc[3] == pytest.approx(2.0 / 3.0 * 5.0 + 1.0 / 3.0 * 2.0)  # 4.0


def test_ema_a_nan_never_counts_towards_a_longer_seed() -> None:
    # With window 2 a NaN wrongly COUNTED as a seed observation happens to give the same seed; with window 3 the seed would complete a value early.
    result = op.ema(_series([1.0, np.nan, 2.0, 3.0, 4.0]), 3)  # alpha = 0.5
    assert result.iloc[:3].isna().all()  # at index 2 only two non-NaN values have arrived
    assert result.iloc[3] == pytest.approx(2.0)  # the mean of 1, 2 and 3
    assert result.iloc[4] == pytest.approx(0.5 * 4.0 + 0.5 * 2.0)  # 3.0


def test_ema_trailing_and_repeated_nans() -> None:
    result = op.ema(_series([2.0, 4.0, np.nan, np.nan, 6.0, np.nan]), 2)
    assert np.isnan(result.iloc[0])
    assert result.iloc[1] == pytest.approx(3.0)
    assert result.iloc[2:4].isna().all()
    assert result.iloc[4] == pytest.approx(2.0 / 3.0 * 6.0 + 1.0 / 3.0 * 3.0)  # 5.0: state held over a two-bar gap
    assert np.isnan(result.iloc[5])


def test_ema_window_one_is_the_input_itself_with_nan_where_the_input_is_nan() -> None:
    result = op.ema(_series([5.0, np.nan, 7.0]), 1)  # alpha = 1
    assert result.iloc[0] == pytest.approx(5.0) and np.isnan(result.iloc[1]) and result.iloc[2] == pytest.approx(7.0)


def _legacy_ema(x: pd.Series, window: int) -> pd.Series:
    """The previous implementation, kept ONLY as the reference for input whose NaNs are a leading prefix."""
    values = x.to_numpy(dtype=np.float64)
    result = np.full(len(values), np.nan)
    start = int(np.argmax(~np.isnan(values))) if (~np.isnan(values)).any() else len(values)
    seed_position = start + window - 1
    if seed_position >= len(values):
        return pd.Series(result, index=x.index)
    alpha = 2.0 / (window + 1)
    result[seed_position] = float(np.mean(values[start : start + window]))
    for i in range(seed_position + 1, len(values)):
        result[i] = alpha * values[i] + (1.0 - alpha) * result[i - 1]
    return pd.Series(result, index=x.index)


@pytest.mark.parametrize("seed", range(8))
def test_ema_is_bit_identical_to_the_previous_result_when_nans_are_only_a_prefix(seed: int) -> None:
    rng = np.random.default_rng(seed)
    n = int(rng.integers(5, 60))
    values = rng.normal(100.0, 5.0, n)
    values[: int(rng.integers(0, 4))] = np.nan  # a leading NaN run only
    for window in (1, 2, 3, 7, 12):
        x = pd.Series(values, index=pd.RangeIndex(n))
        np.testing.assert_array_equal(op.ema(x, window).to_numpy(), _legacy_ema(x, window).to_numpy())


# ---------------------------------------------------------------------------
# stddev
# ---------------------------------------------------------------------------


def test_stddev_hand_verified() -> None:
    x = _series([2, 4, 4, 4, 5, 5, 7, 9])
    result = op.stddev(x, 8)
    expected = float(np.std(x.to_numpy(), ddof=1))
    assert result.iloc[7] == pytest.approx(expected)
    assert result.iloc[:7].isna().all()


def test_stddev_rejects_window_below_two() -> None:
    with pytest.raises(ValueError, match="window"):
        op.stddev(_series([1.0, 2.0, 3.0]), 1)


# ---------------------------------------------------------------------------
# rolling_max / rolling_min
# ---------------------------------------------------------------------------


def test_rolling_max_min_hand_verified() -> None:
    x = _series([3, 1, 4, 1, 5, 9, 2, 6])
    hi = op.rolling_max(x, 3)
    lo = op.rolling_min(x, 3)
    assert hi.iloc[:2].isna().all()
    assert hi.iloc[2] == pytest.approx(4.0)  # max(3,1,4)
    assert hi.iloc[5] == pytest.approx(9.0)  # max(1,5,9)
    assert lo.iloc[2] == pytest.approx(1.0)
    assert lo.iloc[5] == pytest.approx(1.0)  # min(1,5,9)


# ---------------------------------------------------------------------------
# roc
# ---------------------------------------------------------------------------


def test_roc_hand_verified() -> None:
    x = _series([100, 105, 110, 90, 120])
    result = op.roc(x, 2)
    assert result.iloc[:2].isna().all()
    assert result.iloc[2] == pytest.approx(110.0 / 100.0 - 1.0)
    assert result.iloc[4] == pytest.approx(120.0 / 110.0 - 1.0)


def test_roc_zero_denominator_is_nan_not_inf() -> None:
    x = _series([0, 5, 10])
    result = op.roc(x, 1)
    assert np.isnan(result.iloc[1])
    assert not np.isinf(result.to_numpy()[~np.isnan(result.to_numpy())]).any()


# ---------------------------------------------------------------------------
# rsi -- hand-verified against manually computed Wilder averages
# ---------------------------------------------------------------------------


def test_rsi_hand_verified() -> None:
    prices = _series([44.0, 44.25, 44.5, 43.75, 44.65, 45.12, 45.0])
    result = op.rsi(prices, 4)
    assert result.iloc[:4].isna().all()

    deltas = np.diff(prices.to_numpy())
    gains = np.where(deltas > 0, deltas, 0.0)
    losses = np.where(deltas < 0, -deltas, 0.0)
    avg_gain = float(np.mean(gains[:4]))
    avg_loss = float(np.mean(losses[:4]))
    expected_seed = 100.0 - 100.0 / (1.0 + avg_gain / avg_loss)
    assert result.iloc[4] == pytest.approx(expected_seed)

    delta5 = prices.iloc[5] - prices.iloc[4]
    gain5 = delta5 if delta5 > 0 else 0.0
    loss5 = -delta5 if delta5 < 0 else 0.0
    avg_gain = (avg_gain * 3 + gain5) / 4
    avg_loss = (avg_loss * 3 + loss5) / 4
    expected_next = 100.0 - 100.0 / (1.0 + avg_gain / avg_loss)
    assert result.iloc[5] == pytest.approx(expected_next)


def test_rsi_zero_losses_is_100() -> None:
    x = _series([1, 2, 3, 4, 5, 6])  # strictly increasing -> zero losses
    result = op.rsi(x, 4)
    assert result.iloc[4] == pytest.approx(100.0)


def test_rsi_warmup_matches_window_plus_one() -> None:
    # "warm-up = window + 1" (§5.1's table) means window+1 BARS are needed before the first
    # valid value -- i.e. positions 0..window-1 (window positions) are NaN, and position
    # `window` (0-indexed) is the first valid one.
    x = _series(list(range(1, 12)))
    window = 5
    result = op.rsi(x, window)
    assert result.iloc[:window].isna().all()
    assert not np.isnan(result.iloc[window])


def test_rsi_rejects_non_positive_window() -> None:
    with pytest.raises(ValueError, match="window"):
        op.rsi(_series([1.0, 2.0]), 0)


# ---------------------------------------------------------------------------
# lag
# ---------------------------------------------------------------------------


def test_lag_hand_verified() -> None:
    x = _series([10, 20, 30, 40])
    result = op.lag(x, 2)
    assert result.iloc[:2].isna().all()
    assert result.iloc[2] == pytest.approx(10.0)
    assert result.iloc[3] == pytest.approx(20.0)


def test_lag_rejects_non_positive_n() -> None:
    with pytest.raises(ValueError, match="n"):
        op.lag(_series([1.0, 2.0]), 0)


# ---------------------------------------------------------------------------
# arithmetic
# ---------------------------------------------------------------------------


def test_arithmetic_series_series() -> None:
    a = _series([1, 2, 3])
    b = _series([10, 20, 30])
    assert op.add(a, b).tolist() == [11.0, 22.0, 33.0]
    assert op.sub(a, b).tolist() == [-9.0, -18.0, -27.0]
    assert op.mul(a, b).tolist() == [10.0, 40.0, 90.0]
    assert op.div(a, b).tolist() == pytest.approx([0.1, 0.1, 0.1])


def test_arithmetic_series_literal() -> None:
    a = _series([1, 2, 3])
    assert op.add(a, 10.0).tolist() == [11.0, 12.0, 13.0]
    assert op.sub(a, 1.0).tolist() == [0.0, 1.0, 2.0]
    assert op.mul(a, 2.0).tolist() == [2.0, 4.0, 6.0]
    assert op.div(10.0, a).tolist() == pytest.approx([10.0, 5.0, 10.0 / 3.0])


def test_div_by_zero_is_nan_never_inf() -> None:
    a = _series([1.0, 2.0, -3.0])
    b = _series([1.0, 0.0, 0.0])
    result = op.div(a, b)
    assert np.isnan(result.iloc[1])
    assert np.isnan(result.iloc[2])
    assert not np.isinf(result.to_numpy()[~np.isnan(result.to_numpy())]).any()


def test_align_rejects_two_literals() -> None:
    with pytest.raises(TypeError, match="pd.Series"):
        op.add(1.0, 2.0)


def test_align_rejects_mismatched_indices() -> None:
    a = pd.Series([1.0, 2.0], index=[0, 1])
    b = pd.Series([1.0, 2.0], index=[10, 11])
    with pytest.raises(ValueError, match="索引"):
        op.add(a, b)


# ---------------------------------------------------------------------------
# comparisons (encoded as 1.0/0.0/NaN)
# ---------------------------------------------------------------------------


def test_comparisons_hand_verified() -> None:
    a = _series([1, 5, 3])
    b = _series([2, 2, 3])
    assert op.gt(a, b).tolist() == [0.0, 1.0, 0.0]
    assert op.lt(a, b).tolist() == [1.0, 0.0, 0.0]
    assert op.ge(a, b).tolist() == [0.0, 1.0, 1.0]
    assert op.le(a, b).tolist() == [1.0, 0.0, 1.0]


def test_comparisons_propagate_nan_not_false() -> None:
    a = pd.Series([1.0, np.nan, 3.0], index=pd.RangeIndex(3))
    b = _series([2, 2, 2])
    result = op.gt(a, b)
    assert np.isnan(result.iloc[1])  # NOT 0.0 -- NaN must propagate, not silently become False


# ---------------------------------------------------------------------------
# crosses_above / crosses_below
# ---------------------------------------------------------------------------


def test_crosses_above_hand_verified() -> None:
    a = _series([1, 3, 2, 5, 1])
    b = _series([2, 2, 2, 2, 2])
    result = op.crosses_above(a, b)
    assert np.isnan(result.iloc[0])  # no t-1
    assert result.tolist()[1:] == pytest.approx([1.0, 0.0, 1.0, 0.0])


def test_crosses_below_hand_verified() -> None:
    a = _series([3, 1, 2, -1, 3])
    b = _series([2, 2, 2, 2, 2])
    result = op.crosses_below(a, b)
    assert np.isnan(result.iloc[0])
    assert result.tolist()[1:] == pytest.approx([1.0, 0.0, 1.0, 0.0])


def test_crosses_above_nan_input_propagates() -> None:
    a = pd.Series([1.0, np.nan, 3.0], index=pd.RangeIndex(3))
    b = _series([2, 2, 2])
    result = op.crosses_above(a, b)
    assert np.isnan(result.iloc[1])
    assert np.isnan(result.iloc[2])  # needs a[1] (NaN) as its "t-1"


def test_crosses_functions_reject_mismatched_indices() -> None:
    a = pd.Series([1.0, 2.0], index=[0, 1])
    b = pd.Series([1.0, 2.0], index=[10, 11])
    with pytest.raises(ValueError, match="索引"):
        op.crosses_above(a, b)


# ---------------------------------------------------------------------------
# logical and/or/not
# ---------------------------------------------------------------------------


def test_logical_and_or_hand_verified() -> None:
    a = _series([1, 1, 0, 0])
    b = _series([1, 0, 1, 0])
    assert op.logical_and(a, b).tolist() == [1.0, 0.0, 0.0, 0.0]
    assert op.logical_or(a, b).tolist() == [1.0, 1.0, 1.0, 0.0]


def test_logical_not_hand_verified() -> None:
    a = _series([1, 0])
    assert op.logical_not(a).tolist() == [0.0, 1.0]


def test_logical_ops_propagate_nan() -> None:
    a = pd.Series([1.0, np.nan], index=pd.RangeIndex(2))
    b = _series([1, 1])
    assert np.isnan(op.logical_and(a, b).iloc[1])
    assert np.isnan(op.logical_or(a, b).iloc[1])
    assert np.isnan(op.logical_not(a).iloc[1])


# ---------------------------------------------------------------------------
# if_then_else
# ---------------------------------------------------------------------------


def test_if_then_else_hand_verified() -> None:
    cond = _series([1, 0, 1])
    then = _series([10, 20, 30])
    otherwise = _series([100, 200, 300])
    result = op.if_then_else(cond, then, otherwise)
    assert result.tolist() == [10.0, 200.0, 30.0]


def test_if_then_else_with_literal_branches() -> None:
    cond = _series([1, 0, 1])
    result = op.if_then_else(cond, 1.0, 0.0)
    assert result.tolist() == [1.0, 0.0, 1.0]


def test_if_then_else_nan_cond_is_nan_regardless_of_branches() -> None:
    cond = pd.Series([1.0, np.nan, 0.0], index=pd.RangeIndex(3))
    result = op.if_then_else(cond, 1.0, 0.0)
    assert np.isnan(result.iloc[1])


def test_if_then_else_selected_branchs_own_nan_flows_through() -> None:
    # cond is already valid (1.0) at position 0, but `then`'s own warm-up hasn't finished --
    # the selected NaN should flow through untouched, not be silently replaced.
    cond = _series([1, 1])
    then = pd.Series([np.nan, 5.0], index=pd.RangeIndex(2))
    otherwise = _series([100, 200])
    result = op.if_then_else(cond, then, otherwise)
    assert np.isnan(result.iloc[0])
    assert result.iloc[1] == pytest.approx(5.0)
