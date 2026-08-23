"""Evaluates a parsed StrategySpec DAG against an OHLCV DataFrame — 批次 3, round 2.

Trusts everything `schema.py` already validated (known operator names, forward-reference-
only DAG, correct required fields per operator shape) -- this module is pure numerical
evaluation, walking `spec.indicators` in declared order (already guaranteed a valid
topological order by `schema.py`'s forward-reference check, so no separate ordering/cycle
logic is needed here either).
"""

from __future__ import annotations

from typing import Any

import numpy as np
import pandas as pd

from py_core.indicators import operators
from py_core.manual_ohlcv import NormalizedOhlcvRecord
from py_core.strategy_spec import schema

_WINDOWED_FUNCS = {
    "sma": operators.sma,
    "ema": operators.ema,
    "stddev": operators.stddev,
    "rolling_max": operators.rolling_max,
    "rolling_min": operators.rolling_min,
    "roc": operators.roc,
    "rsi": operators.rsi,
}
_CROSSES_FUNCS = {
    "crosses_above": operators.crosses_above,
    "crosses_below": operators.crosses_below,
}
_BINARY_FUNCS = {
    "add": operators.add,
    "sub": operators.sub,
    "mul": operators.mul,
    "div": operators.div,
    "gt": operators.gt,
    "lt": operators.lt,
    "ge": operators.ge,
    "le": operators.le,
    "and": operators.logical_and,
    "or": operators.logical_or,
}


def _own_warmup(node: schema.IndicatorNode) -> int:
    """§6: each operator's own warm-up contribution, before adding its inputs' warm-up."""
    if node.op == "rsi":
        assert node.param is not None
        return node.param + 1
    if node.op in schema._WINDOW_OPS or node.op == schema._LAG_OP:
        assert node.param is not None
        return node.param
    if node.op in _CROSSES_FUNCS:
        return 1  # needs t AND t-1, one more bar than its operands' own warm-up
    return 0  # arithmetic/comparison/logical/if_then_else contribute none of their own


def _resolve_operand(
    value: schema.OperandValue, node_values: dict[str, pd.Series], df: pd.DataFrame
) -> pd.Series | float:
    if isinstance(value, str):
        if value in schema.RAW_FIELDS:
            return df[value].astype(np.float64)
        return node_values[value]
    return value


def _resolve_series(
    value: schema.OperandValue, node_values: dict[str, pd.Series], df: pd.DataFrame
) -> pd.Series:
    resolved = _resolve_operand(value, node_values, df)
    if isinstance(resolved, pd.Series):
        return resolved
    return pd.Series(resolved, index=df.index, dtype=np.float64)


def _evaluate_node(node: schema.IndicatorNode, node_values: dict[str, pd.Series], df: pd.DataFrame) -> pd.Series:
    op = node.op
    if op in _WINDOWED_FUNCS:
        x = _resolve_series(node.operands["input"], node_values, df)
        assert node.param is not None
        return _WINDOWED_FUNCS[op](x, node.param)
    if op == schema._LAG_OP:
        x = _resolve_series(node.operands["input"], node_values, df)
        assert node.param is not None
        return operators.lag(x, node.param)
    if op == "not":
        x = _resolve_series(node.operands["input"], node_values, df)
        return operators.logical_not(x)
    if op in _CROSSES_FUNCS:
        a = _resolve_series(node.operands["left"], node_values, df)
        b = _resolve_series(node.operands["right"], node_values, df)
        return _CROSSES_FUNCS[op](a, b)
    if op in _BINARY_FUNCS:
        left = _resolve_operand(node.operands["left"], node_values, df)
        right = _resolve_operand(node.operands["right"], node_values, df)
        return _BINARY_FUNCS[op](left, right)
    if op == "if_then_else":
        cond = _resolve_series(node.operands["cond"], node_values, df)
        then = _resolve_operand(node.operands["then"], node_values, df)
        otherwise = _resolve_operand(node.operands["otherwise"], node_values, df)
        return operators.if_then_else(cond, then, otherwise)
    raise schema.StrategySpecError(  # pragma: no cover -- unreachable given schema.KNOWN_OPS, protects against this file's own dispatch table drifting out of sync with schema.py's
        f"未知算子: {op!r}"
    )


def evaluate_spec(spec: schema.StrategySpecDoc, df: pd.DataFrame) -> pd.Series[Any]:
    """Evaluate every ``[[indicators]]`` node in declared order, then apply ``[signal]``'s
    mode to produce the final target-position Series (§4.4).

    Args:
        spec: a schema.parse_spec()/load_spec() result.
        df: OHLCV DataFrame in ``records_to_dataframe()``'s shape.

    Returns:
        A Series aligned to ``df.index``, values in ``{0.0, 1.0}`` (``mode="boolean"``) or
        ``[-1.0, 1.0]`` (``mode="scaled"``) -- never NaN (both modes map warm-up/undefined to
        ``0.0``, per §4.4).
    """
    node_values: dict[str, pd.Series] = {}
    for node in spec.indicators:
        node_values[node.id] = _evaluate_node(node, node_values, df)

    signal_series = node_values[spec.signal.node]
    if spec.signal.mode == "boolean":
        arr = signal_series.to_numpy(dtype=np.float64)
        result = np.where(np.isnan(arr), 0.0, np.where(arr != 0.0, 1.0, 0.0))
        return pd.Series(result, index=df.index)
    if spec.signal.mode == "scaled":
        return signal_series.clip(lower=-1.0, upper=1.0).fillna(0.0)
    raise schema.StrategySpecError(f"未知 signal mode: {spec.signal.mode!r}")  # pragma: no cover -- schema validated


def compute_effective_warmup(spec: schema.StrategySpecDoc) -> int:
    """§6: static warm-up computation -- how many leading bars the ``[signal]`` node's own
    value is guaranteed NaN for, computed purely from the DAG structure (no data needed).
    """
    warmup_by_id: dict[str, int] = {}
    for node in spec.indicators:
        input_warmups = [
            warmup_by_id[ref]
            for ref in node.operands.values()
            if isinstance(ref, str) and ref not in schema.RAW_FIELDS
        ]
        warmup_by_id[node.id] = _own_warmup(node) + (max(input_warmups) if input_warmups else 0)
    return warmup_by_id.get(spec.signal.node, 0)


def validate_market_binding(spec: schema.StrategySpecDoc, records: list[NormalizedOhlcvRecord]) -> None:
    """Reject if any record's market/symbol/timeframe doesn't match ``spec.market`` --
    "绑定是强制的、执行期要核对的" (§4.2). Caller's responsibility to invoke before
    evaluating a spec against real records: ``evaluate_spec()``/``SpecStrategy.generate_signals()``
    only ever see a plain OHLCV DataFrame with no market metadata attached, so neither can
    check this themselves.

    Raises:
        schema.StrategySpecError: any record's market/symbol/timeframe doesn't match.
    """
    for r in records:
        if r.market != spec.market.market or r.symbol != spec.market.symbol or r.timeframe != spec.market.timeframe:
            raise schema.StrategySpecError(
                f"记录 (market={r.market!r}, symbol={r.symbol!r}, timeframe={r.timeframe!r}) 跟 "
                f"spec 的 [market] 绑定 (market={spec.market.market!r}, symbol={spec.market.symbol!r}, "
                f"timeframe={spec.market.timeframe!r}) 不一致"
            )
