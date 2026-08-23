"""Parsing and structural validation for `docs/STRATEGY_SPEC.md`'s TOML format — 批次 3,
round 2.

Structural validity (this module) is deliberately separated from numerical evaluation
(`evaluator.py`): a spec that parses successfully here is guaranteed to have a well-formed
DAG (every operand reference resolves to either a raw OHLCV field or an EARLIER-declared
node -- §2.2's "只能向前引用...不需要单独的环检测" enforced once, here, rather than trusted
by every downstream consumer), every operator's required fields present with the right
shape, and a known operator name -- so `evaluator.py` never needs to re-check any of that
and can assume it's evaluating something well-formed.

Uses the stdlib `tomllib` (Python 3.11+, no new dependency -- `py_core` already requires
3.13).
"""

from __future__ import annotations

import re
import tomllib
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from py_core.manual_ohlcv import CanonicalMarketSymbol, ManualMarket, OhlcvTimeframe

RAW_FIELDS = frozenset({"open", "high", "low", "close", "volume"})

_NAME_PATTERN = re.compile(r"^[a-z0-9_]{1,64}$")
_ID_PATTERN = re.compile(r"^[a-z0-9_]{1,32}$")

_WINDOW_OPS = frozenset({"sma", "ema", "stddev", "rolling_max", "rolling_min", "roc", "rsi"})
_LAG_OP = "lag"
_BINARY_VALUE_OPS = frozenset(
    {"add", "sub", "mul", "div", "gt", "lt", "ge", "le", "and", "or", "crosses_above", "crosses_below"}
)
_UNARY_VALUE_OP = "not"
_TERNARY_OP = "if_then_else"

KNOWN_OPS = _WINDOW_OPS | {_LAG_OP, _UNARY_VALUE_OP, _TERNARY_OP} | _BINARY_VALUE_OPS

OperandValue = str | float


class StrategySpecError(ValueError):
    """Raised for any structurally invalid spec -- malformed TOML, unknown operator,
    missing/mis-typed field, forward-reference violation, duplicate id, etc."""


def _operand_field_names(op: str) -> tuple[str, ...]:
    if op in _WINDOW_OPS or op == _LAG_OP or op == _UNARY_VALUE_OP:
        return ("input",)
    if op in _BINARY_VALUE_OPS:
        return ("left", "right")
    if op == _TERNARY_OP:
        return ("cond", "then", "otherwise")
    raise StrategySpecError(f"未知算子: {op!r}")


def _string_only_operand_fields(op: str) -> frozenset[str]:
    """Fields that must reference a raw field or node id -- a bare literal here would break
    the operator function's own signature (e.g. `if_then_else`'s `cond` calls
    `cond.to_numpy()`, `crosses_above`'s operands both need a genuine Series)."""
    if op in _WINDOW_OPS or op == _LAG_OP or op == _UNARY_VALUE_OP:
        return frozenset({"input"})
    if op == _TERNARY_OP:
        return frozenset({"cond"})
    return frozenset()


def _param_field_name(op: str) -> str | None:
    if op in _WINDOW_OPS:
        return "window"
    if op == _LAG_OP:
        return "n"
    return None


@dataclass(frozen=True, slots=True)
class MarketBinding:
    market: ManualMarket
    symbol: CanonicalMarketSymbol
    timeframe: OhlcvTimeframe


@dataclass(frozen=True, slots=True)
class IndicatorNode:
    """One `[[indicators]]` entry. ``operands`` holds only the operand fields (§ op-shape
    dependent: ``input``, or ``left``/``right``, or ``cond``/``then``/``otherwise``) as
    ``str`` (raw field name or node id) or ``float`` (literal); ``param`` holds the single
    ``window``/``n`` integer parameter for ops that have one, else ``None``."""

    id: str
    op: str
    operands: dict[str, OperandValue]
    param: int | None


@dataclass(frozen=True, slots=True)
class SignalConfig:
    node: str
    mode: str  # "boolean" | "scaled"


@dataclass(frozen=True, slots=True)
class ValidationRecord:
    """`[validation]` -- see docs/STRATEGY_SPEC.md §4.5. Optional at parse time: this
    Python evaluator is itself one of the tools round 3's parameter sweep runs to PRODUCE
    this section, so a spec without one yet must still parse and evaluate (the "C++ 求值器
    必须拒绝加载缺少本节...的 spec" requirement is stated for the execution-side evaluator,
    batch 5's job, not this research-side one)."""

    validated_at: str
    validated_by: str
    data_start_utc: str
    data_end_utc: str
    data_digest: str
    oos_sharpe: float
    pbo: float
    trials: int


@dataclass(frozen=True, slots=True)
class StrategySpecDoc:
    spec_version: int
    name: str
    description: str | None
    market: MarketBinding
    indicators: tuple[IndicatorNode, ...]
    signal: SignalConfig
    validation: ValidationRecord | None


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise StrategySpecError(message)


def _require_table(doc: dict[str, Any], key: str) -> dict[str, Any]:
    value = doc.get(key)
    _require(isinstance(value, dict), f"[{key}] 是必填表格")
    return value


def _parse_market(doc: dict[str, Any]) -> MarketBinding:
    table = _require_table(doc, "market")
    for field in ("market", "symbol", "timeframe"):
        _require(field in table, f"[market] 缺少必填字段 {field!r}")
    try:
        market = ManualMarket(table["market"])
    except ValueError as exc:
        raise StrategySpecError(f"[market].market 不是合法的 ManualMarket 取值: {table['market']!r}") from exc
    try:
        symbol = CanonicalMarketSymbol(str(table["symbol"]))
    except ValueError as exc:
        raise StrategySpecError(f"[market].symbol 不合法: {table['symbol']!r} ({exc})") from exc
    try:
        timeframe = OhlcvTimeframe(str(table["timeframe"]))
    except ValueError as exc:
        raise StrategySpecError(f"[market].timeframe 不合法: {table['timeframe']!r} ({exc})") from exc
    return MarketBinding(market=market, symbol=symbol, timeframe=timeframe)


def _parse_operand_value(raw: Any, *, field: str, node_id: str, string_only: bool) -> OperandValue:
    if isinstance(raw, bool):  # bool is a subclass of int in Python -- reject before the int/float check
        raise StrategySpecError(f"indicator {node_id!r} 字段 {field!r} 不能是布尔值")
    if isinstance(raw, str):
        return raw
    if isinstance(raw, (int, float)):
        if string_only:
            raise StrategySpecError(f"indicator {node_id!r} 字段 {field!r} 必须引用一个节点/原始字段，不能是字面量")
        return float(raw)
    raise StrategySpecError(f"indicator {node_id!r} 字段 {field!r} 必须是字符串或数字，收到 {type(raw).__name__}")


def _parse_indicators(doc: dict[str, Any]) -> tuple[IndicatorNode, ...]:
    raw_indicators = doc.get("indicators")
    _require(isinstance(raw_indicators, list) and len(raw_indicators) > 0, "[[indicators]] 至少需要一个节点")

    seen_ids: set[str] = set()
    nodes: list[IndicatorNode] = []
    for i, raw_node in enumerate(raw_indicators):
        _require(isinstance(raw_node, dict), f"indicators[{i}] 必须是表格")
        node_id = raw_node.get("id")
        _require(isinstance(node_id, str) and bool(_ID_PATTERN.fullmatch(node_id)), f"indicators[{i}].id 不合法（必须匹配 ^[a-z0-9_]{{1,32}}$）: {node_id!r}")
        _require(node_id not in RAW_FIELDS, f"indicators[{i}].id {node_id!r} 跟原始字段名冲突（open/high/low/close/volume 保留）")
        _require(node_id not in seen_ids, f"indicators[{i}].id {node_id!r} 重复")

        op = raw_node.get("op")
        _require(op in KNOWN_OPS, f"indicators[{i}]（id={node_id!r}）的 op 不是已知算子: {op!r}")

        operand_fields = _operand_field_names(op)
        string_only = _string_only_operand_fields(op)
        operands: dict[str, OperandValue] = {}
        for field in operand_fields:
            _require(field in raw_node, f"indicator {node_id!r}（op={op!r}）缺少必填字段 {field!r}")
            operands[field] = _parse_operand_value(
                raw_node[field], field=field, node_id=node_id, string_only=field in string_only
            )
            ref = operands[field]
            if isinstance(ref, str) and ref not in RAW_FIELDS:
                _require(
                    ref in seen_ids,
                    f"indicator {node_id!r} 字段 {field!r} 引用了 {ref!r}，但它不是原始字段也不是"
                    "更早声明过的节点 id（只能向前引用，见 STRATEGY_SPEC.md §2.2）",
                )

        param_field = _param_field_name(op)
        param: int | None = None
        if param_field is not None:
            _require(param_field in raw_node, f"indicator {node_id!r}（op={op!r}）缺少必填字段 {param_field!r}")
            raw_param = raw_node[param_field]
            _require(
                isinstance(raw_param, int) and not isinstance(raw_param, bool),
                f"indicator {node_id!r} 字段 {param_field!r} 必须是整数，收到 {raw_param!r}",
            )
            param = raw_param

        nodes.append(IndicatorNode(id=node_id, op=op, operands=operands, param=param))
        seen_ids.add(node_id)

    return tuple(nodes)


def _parse_signal(doc: dict[str, Any], known_ids: frozenset[str]) -> SignalConfig:
    table = _require_table(doc, "signal")
    node = table.get("node")
    _require(isinstance(node, str) and node in known_ids, f"[signal].node 必须引用一个已声明的 indicator id，收到 {node!r}")
    mode = table.get("mode")
    _require(mode in ("boolean", "scaled"), f"[signal].mode 必须是 'boolean' 或 'scaled'，收到 {mode!r}")
    return SignalConfig(node=node, mode=mode)


def _parse_validation(doc: dict[str, Any]) -> ValidationRecord | None:
    table = doc.get("validation")
    if table is None:
        return None
    _require(isinstance(table, dict), "[validation] 必须是表格")
    required = (
        "validated_at",
        "validated_by",
        "data_start_utc",
        "data_end_utc",
        "data_digest",
        "oos_sharpe",
        "pbo",
        "trials",
    )
    for field in required:
        _require(field in table, f"[validation] 缺少字段 {field!r}")
    return ValidationRecord(
        validated_at=str(table["validated_at"]),
        validated_by=str(table["validated_by"]),
        data_start_utc=str(table["data_start_utc"]),
        data_end_utc=str(table["data_end_utc"]),
        data_digest=str(table["data_digest"]),
        oos_sharpe=float(table["oos_sharpe"]),
        pbo=float(table["pbo"]),
        trials=int(table["trials"]),
    )


def parse_spec(toml_text: str) -> StrategySpecDoc:
    """Parse and structurally validate a StrategySpec TOML document.

    Raises:
        StrategySpecError: any structural violation (see module docstring).
        tomllib.TOMLDecodeError: the text is not valid TOML at all.
    """
    doc = tomllib.loads(toml_text)

    spec_version = doc.get("spec_version")
    _require(spec_version == 1, f"spec_version 必须是 1，收到 {spec_version!r}")

    name = doc.get("name")
    _require(isinstance(name, str) and bool(_NAME_PATTERN.fullmatch(name)), f"name 不合法（必须匹配 ^[a-z0-9_]{{1,64}}$）: {name!r}")

    description = doc.get("description")
    if description is not None:
        _require(isinstance(description, str), "description 必须是字符串")

    market = _parse_market(doc)
    indicators = _parse_indicators(doc)
    known_ids = frozenset(node.id for node in indicators)
    signal = _parse_signal(doc, known_ids)
    validation = _parse_validation(doc)

    return StrategySpecDoc(
        spec_version=spec_version,
        name=name,
        description=description,
        market=market,
        indicators=indicators,
        signal=signal,
        validation=validation,
    )


def load_spec(path: str | Path) -> StrategySpecDoc:
    """Read and parse a StrategySpec TOML file. See ``parse_spec()``."""
    return parse_spec(Path(path).read_text(encoding="utf-8"))
