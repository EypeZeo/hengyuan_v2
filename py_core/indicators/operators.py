"""Vectorized implementations of ``docs/STRATEGY_SPEC.md`` §5's operator set — 批次 3, round 1.

This is the Python side of a computation whose exact semantics are meant to be reproduced
bit-for-bit by a future C++ streaming evaluator (batch 5's job); §2.2 of the spec pins the
*method*, not just the result, for exactly that reason ("数学上等价"不够 -- floating-point
accumulation order matters over long series). This module exists to be the single place
those pinned semantics live in Python; nothing here should be "simplified" toward a faster
but differently-accumulating pandas built-in without checking §2.2 first.

**Every value flowing through this module is a ``pd.Series[float64]``, including
boolean-valued nodes** (comparisons, ``crosses_above``/``crosses_below``, ``and``/``or``/
``not``): ``True`` = ``1.0``, ``False`` = ``0.0``, undefined = ``NaN`` -- per §2.2's "所有
中间量一律 float64", there is no separate boolean dtype anywhere in a StrategySpec DAG.
``NodeInput`` (``pd.Series | float``) covers the fact that many operator arguments (``right``
in ``add``/``gt``/etc., ``then``/``otherwise`` in ``if_then_else``) may be either a DAG node
reference (always resolved to a ``pd.Series`` by round 2's evaluator before reaching here) or
a literal number straight from the spec.

**NaN-prefix invariant, relied on by ``ema()``/``rsi()``'s warm-up composition:** every
operator here produces NaN only as a *contiguous prefix* of its output, never a NaN appearing
after the first non-NaN value -- true by construction for every operator in this module
(rolling ops via pandas' ``min_periods`` semantics, which count non-NaN *observations* in the
window rather than window length, so a NaN-prefixed input composes correctly automatically;
elementwise ops via NaN-if-any-input-NaN, which preserves "prefix" under union). §6's warm-up
formula ("自身 warm-up + 输入 warm-up 的最大值，逐层向上累加") is only well-defined if this
invariant holds throughout the DAG -- ``ema()``/``rsi()`` explicitly find and skip their
input's own leading NaN run (via ``_first_valid_position()``) rather than assuming the input
starts at position 0, specifically so the invariant keeps holding through operators that
can't use pandas' built-in rolling ``min_periods`` machinery (their own state is a genuine
recursion, not a pandas-native rolling reduction).

**Division and ratio operators never produce ``inf``**: §5.3 mandates ``div`` produce NaN
(not raise, not inf) when the right operand is 0 -- "两侧行为必须一致，NaN 会被 `[signal]`
当作空仓处理". ``roc()`` applies the identical rule to its own implicit division for the same
reason (an ``inf`` anywhere in this system has no defined downstream meaning; only NaN does).
"""

from __future__ import annotations

import numpy as np
import pandas as pd

NodeInput = pd.Series | float


def _first_valid_position(values: np.ndarray) -> int:
    """Position of the first non-NaN entry, or ``len(values)`` if all-NaN."""
    valid = ~np.isnan(values)
    if not valid.any():
        return len(values)
    return int(np.argmax(valid))


def _align(left: NodeInput, right: NodeInput) -> tuple[np.ndarray, np.ndarray, pd.Index]:
    """Broadcast a (Series, Series) or (Series, literal) pair to a pair of same-length
    float64 numpy arrays sharing one index. At least one side must be a ``pd.Series`` --
    two literals together are a degenerate, unused-in-practice spec shape (every real
    operator invocation references at least one DAG node), so this raises rather than
    silently returning a same-valued pair with no index to attach to a result Series.

    Raises:
        TypeError: neither left nor right is a pd.Series.
        ValueError: both are pd.Series with different indices.
    """
    if isinstance(left, pd.Series) and isinstance(right, pd.Series):
        if not left.index.equals(right.index):
            raise ValueError("left/right 的索引必须完全一致")
        return left.to_numpy(dtype=np.float64), right.to_numpy(dtype=np.float64), left.index
    if isinstance(left, pd.Series):
        return left.to_numpy(dtype=np.float64), np.full(len(left), float(right)), left.index
    if isinstance(right, pd.Series):
        return np.full(len(right), float(left)), right.to_numpy(dtype=np.float64), right.index
    raise TypeError("left 和 right 至少一个必须是 pd.Series")


# ---------------------------------------------------------------------------
# §5.1 窗口类
# ---------------------------------------------------------------------------


def sma(x: pd.Series, window: int) -> pd.Series:
    """§2.2: window-recomputed every bar, NOT a sliding add/subtract -- ``.apply()`` calls
    ``np.mean()`` fresh on each window's raw values, so there is no incrementally
    accumulated rounding error to depend on the historical path."""
    if window < 1:
        raise ValueError(f"window must be >= 1, got {window}")
    return x.rolling(window, min_periods=window).apply(lambda arr: float(np.mean(arr)), raw=True)


def ema(x: pd.Series, window: int) -> pd.Series:
    """§2.2: ``ema[t] = alpha*x[t] + (1-alpha)*ema[t-1]``, ``alpha = 2/(window+1)``, seeded
    with ``SMA(x[0..warmup-1])`` (NOT ``x[0]``) -- both pinned exactly by the spec since seed
    choice is the most common source of divergence between EMA implementations."""
    if window < 1:
        raise ValueError(f"window must be >= 1, got {window}")
    values = x.to_numpy(dtype=np.float64)
    n = len(values)
    result = np.full(n, np.nan)
    start = _first_valid_position(values)
    seed_position = start + window - 1
    if seed_position >= n:
        return pd.Series(result, index=x.index)

    alpha = 2.0 / (window + 1)
    result[seed_position] = float(np.mean(values[start : start + window]))
    for i in range(seed_position + 1, n):
        result[i] = alpha * values[i] + (1.0 - alpha) * result[i - 1]
    return pd.Series(result, index=x.index)


def stddev(x: pd.Series, window: int) -> pd.Series:
    """Sample standard deviation (``ddof=1``, per §5.1's table), window-recomputed for the
    same reason as ``sma()`` -- an incremental variance recursion has the same path-dependent
    rounding-error property §2.2 rejects for SMA, so this is deliberately consistent with it
    even though the spec only calls SMA out by name."""
    if window < 2:
        raise ValueError(f"window must be >= 2 (sample stddev needs ddof=1), got {window}")
    return x.rolling(window, min_periods=window).apply(lambda arr: float(np.std(arr, ddof=1)), raw=True)


def rolling_max(x: pd.Series, window: int) -> pd.Series:
    if window < 1:
        raise ValueError(f"window must be >= 1, got {window}")
    return x.rolling(window, min_periods=window).max()


def rolling_min(x: pd.Series, window: int) -> pd.Series:
    if window < 1:
        raise ValueError(f"window must be >= 1, got {window}")
    return x.rolling(window, min_periods=window).min()


def roc(x: pd.Series, window: int) -> pd.Series:
    """``x[t] / x[t-window] - 1``. Denominator 0 -> NaN (see module docstring)."""
    if window < 1:
        raise ValueError(f"window must be >= 1, got {window}")
    x_arr = x.to_numpy(dtype=np.float64)
    shifted_arr = x.shift(window).to_numpy(dtype=np.float64)
    with np.errstate(divide="ignore", invalid="ignore"):
        result = x_arr / shifted_arr - 1.0
    result = np.where(shifted_arr == 0.0, np.nan, result)
    return pd.Series(result, index=x.index)


def _rsi_from_averages(avg_gain: float, avg_loss: float) -> float:
    if avg_loss == 0.0:
        return 100.0
    rs = avg_gain / avg_loss
    return 100.0 - 100.0 / (1.0 + rs)


def rsi(x: pd.Series, window: int) -> pd.Series:
    """Wilder-smoothed RSI (§5.1: "用 Wilder 平滑（alpha = 1/window），**不是** EMA 的
    alpha") -- a genuinely different recursion from ``ema()``, not the same formula with a
    different alpha plugged in: the seed is the mean of the first ``window`` price deltas
    (needing ``window + 1`` prices, matching the table's stated warm-up), and every
    subsequent step re-weights the running average by ``(window - 1)/window`` rather than
    EMA's ``1 - alpha``. ``avg_loss == 0`` -> RSI = 100 (by convention; matches how most
    published implementations treat "no losses observed" regardless of avg_gain)."""
    if window < 1:
        raise ValueError(f"window must be >= 1, got {window}")
    values = x.to_numpy(dtype=np.float64)
    n = len(values)
    result = np.full(n, np.nan)
    start = _first_valid_position(values)
    seed_position = start + window
    if seed_position >= n:
        return pd.Series(result, index=x.index)

    deltas = np.diff(values[start : start + window + 1])
    gains = np.where(deltas > 0, deltas, 0.0)
    losses = np.where(deltas < 0, -deltas, 0.0)
    avg_gain = float(np.mean(gains))
    avg_loss = float(np.mean(losses))
    result[seed_position] = _rsi_from_averages(avg_gain, avg_loss)

    for i in range(seed_position + 1, n):
        delta = values[i] - values[i - 1]
        gain = max(0.0, delta)
        loss = -delta if delta < 0.0 else 0.0
        avg_gain = (avg_gain * (window - 1) + gain) / window
        avg_loss = (avg_loss * (window - 1) + loss) / window
        result[i] = _rsi_from_averages(avg_gain, avg_loss)

    return pd.Series(result, index=x.index)


# ---------------------------------------------------------------------------
# §5.2 移位类
# ---------------------------------------------------------------------------


def lag(x: pd.Series, n: int) -> pd.Series:
    if n < 1:
        raise ValueError(f"n must be >= 1, got {n}")
    return x.shift(n)


# ---------------------------------------------------------------------------
# §5.3 二元算术
# ---------------------------------------------------------------------------


def add(left: NodeInput, right: NodeInput) -> pd.Series:
    l, r, idx = _align(left, right)
    return pd.Series(l + r, index=idx)


def sub(left: NodeInput, right: NodeInput) -> pd.Series:
    l, r, idx = _align(left, right)
    return pd.Series(l - r, index=idx)


def mul(left: NodeInput, right: NodeInput) -> pd.Series:
    l, r, idx = _align(left, right)
    return pd.Series(l * r, index=idx)


def div(left: NodeInput, right: NodeInput) -> pd.Series:
    """Right operand 0 -> NaN (see module docstring), never inf/-inf."""
    l, r, idx = _align(left, right)
    with np.errstate(divide="ignore", invalid="ignore"):
        result = l / r
    result = np.where(r == 0.0, np.nan, result)
    return pd.Series(result, index=idx)


# ---------------------------------------------------------------------------
# §5.4 二元比较 → 布尔（编码为 1.0/0.0/NaN，见模块文档）
# ---------------------------------------------------------------------------


def gt(left: NodeInput, right: NodeInput) -> pd.Series:
    l, r, idx = _align(left, right)
    with np.errstate(invalid="ignore"):
        result = np.where(l > r, 1.0, 0.0)
    result = np.where(np.isnan(l) | np.isnan(r), np.nan, result)
    return pd.Series(result, index=idx)


def lt(left: NodeInput, right: NodeInput) -> pd.Series:
    l, r, idx = _align(left, right)
    with np.errstate(invalid="ignore"):
        result = np.where(l < r, 1.0, 0.0)
    result = np.where(np.isnan(l) | np.isnan(r), np.nan, result)
    return pd.Series(result, index=idx)


def ge(left: NodeInput, right: NodeInput) -> pd.Series:
    l, r, idx = _align(left, right)
    with np.errstate(invalid="ignore"):
        result = np.where(l >= r, 1.0, 0.0)
    result = np.where(np.isnan(l) | np.isnan(r), np.nan, result)
    return pd.Series(result, index=idx)


def le(left: NodeInput, right: NodeInput) -> pd.Series:
    l, r, idx = _align(left, right)
    with np.errstate(invalid="ignore"):
        result = np.where(l <= r, 1.0, 0.0)
    result = np.where(np.isnan(l) | np.isnan(r), np.nan, result)
    return pd.Series(result, index=idx)


def crosses_above(a: pd.Series, b: pd.Series) -> pd.Series:
    """``a[t] > b[t] and a[t-1] <= b[t-1]``, true only on the bar the cross happens.
    NaN if any of the four needed values (both series at t and t-1) is NaN -- including
    position 0, which has no t-1 at all ("warm-up 需要多一根 bar", §5.4)."""
    if not a.index.equals(b.index):
        raise ValueError("a/b 的索引必须完全一致")
    a_shift = a.shift(1)
    b_shift = b.shift(1)
    crossed = ((a > b) & (a_shift <= b_shift)).astype(np.float64)
    any_nan = a.isna() | b.isna() | a_shift.isna() | b_shift.isna()
    return crossed.where(~any_nan, other=np.nan)


def crosses_below(a: pd.Series, b: pd.Series) -> pd.Series:
    """``a[t] < b[t] and a[t-1] >= b[t-1]``. See ``crosses_above()``."""
    if not a.index.equals(b.index):
        raise ValueError("a/b 的索引必须完全一致")
    a_shift = a.shift(1)
    b_shift = b.shift(1)
    crossed = ((a < b) & (a_shift >= b_shift)).astype(np.float64)
    any_nan = a.isna() | b.isna() | a_shift.isna() | b_shift.isna()
    return crossed.where(~any_nan, other=np.nan)


# ---------------------------------------------------------------------------
# §5.5 逻辑（NaN 完全传播 -- 任一操作数未定义，结果就未定义，不做三值逻辑短路）
# ---------------------------------------------------------------------------


def logical_and(left: NodeInput, right: NodeInput) -> pd.Series:
    l, r, idx = _align(left, right)
    result = np.where((l != 0.0) & (r != 0.0), 1.0, 0.0)
    result = np.where(np.isnan(l) | np.isnan(r), np.nan, result)
    return pd.Series(result, index=idx)


def logical_or(left: NodeInput, right: NodeInput) -> pd.Series:
    l, r, idx = _align(left, right)
    result = np.where((l != 0.0) | (r != 0.0), 1.0, 0.0)
    result = np.where(np.isnan(l) | np.isnan(r), np.nan, result)
    return pd.Series(result, index=idx)


def logical_not(x: pd.Series) -> pd.Series:
    arr = x.to_numpy(dtype=np.float64)
    result = np.where(arr != 0.0, 0.0, 1.0)
    result = np.where(np.isnan(arr), np.nan, result)
    return pd.Series(result, index=x.index)


# ---------------------------------------------------------------------------
# §5.6 三元
# ---------------------------------------------------------------------------


def if_then_else(cond: pd.Series, then: NodeInput, otherwise: NodeInput) -> pd.Series:
    """``cond`` NaN -> NaN (undefined which branch to take). Otherwise selects ``then`` or
    ``otherwise`` elementwise -- if the SELECTED branch's own value happens to be NaN (its
    own warm-up not yet satisfied even though ``cond`` already is), that NaN flows through
    naturally with no extra masking needed."""
    cond_arr = cond.to_numpy(dtype=np.float64)
    then_arr, _, _ = _align(then, cond)
    otherwise_arr, _, _ = _align(otherwise, cond)
    with np.errstate(invalid="ignore"):
        result = np.where(cond_arr == 1.0, then_arr, otherwise_arr)
    result = np.where(np.isnan(cond_arr), np.nan, result)
    return pd.Series(result, index=cond.index)
