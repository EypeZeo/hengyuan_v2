"""Parameter sweep + `[validation]` backfill — 批次 3, round 3.

Closes the loop `docs/STRATEGY_SPEC.md` §8 promised batch 3 would: sweep a base spec's
numeric fields over a grid, run batch 2's whole validation toolkit (walk-forward, CPCV, PBO)
over that grid, and write the resulting `[validation]` section back into the winning spec's
TOML text -- "策略产能" becomes concrete here: write a spec with a parameter grid, get a
walk-forward-validated Sharpe and an honest overfitting-probability estimate back, without
hand-writing a Python Strategy subclass.

**Why sweeping produces `{"parsed_spec": variant}` param_grid entries, not the usual
`{"window": ..., ...}` shape.** Round 2's `SpecStrategy.__init__(*, spec_path=None,
parsed_spec=None)` already accepts a fully-built `StrategySpecDoc` as a constructor kwarg --
so a sweep candidate IS `{"parsed_spec": <a StrategySpecDoc with certain fields overridden>}`, and
`run_walk_forward_analysis()`/`run_cpcv_analysis()`/`compute_pbo()` need no modification
whatsoever to consume it: they already just do ``load_strategy(spec, **params)`` for
whatever kwargs the caller's `param_grid` supplies. This is the payoff of round 2's decision
to accept a pre-parsed spec object directly, not just a file path.

**How the winning variant is chosen vs. how `oos_sharpe`/`pbo` are computed --
deliberately different procedures, not an oversight.** The variant that ships (whose
`[validation]` gets filled in) is picked by ONE full-sample in-sample grid search (the
number an analyst tuning by hand would actually optimize) -- but `oos_sharpe` comes from
`run_walk_forward_analysis()`'s own honest per-fold selection (which may pick a DIFFERENT
variant on each fold), and `pbo` comes from `compute_pbo()`'s CSCV combinations over the
WHOLE grid. This is intentional: `oos_sharpe`/`pbo` characterize how trustworthy the
SELECTION PROCEDURE is (do walk-forward folds and CSCV combinations keep landing on
similarly-good variants, or does the in-sample winner change unpredictably and perform badly
out-of-sample), not merely re-report the single winning variant's own backtest number, which
is exactly what these tools exist to guard against reporting naively.
"""

from __future__ import annotations

import dataclasses
import hashlib
import itertools
import math
import re
from collections.abc import Callable
from dataclasses import dataclass
from datetime import UTC, datetime
from typing import Any

from py_core.backtests.models import BacktestConfig, BacktestMetrics
from py_core.backtests.vectorized_engine import (
    _run_vectorized_backtest_on_df,
    records_to_dataframe,
    resolve_annualization_factor,
)
from py_core.manual_ohlcv import NormalizedOhlcvRecord
from py_core.strategies.spec_strategy import SpecStrategy
from py_core.strategy_spec.schema import StrategySpecDoc, ValidationRecord
from py_core.validation.deflated_sharpe import compute_deflated_sharpe_ratio
from py_core.validation.pbo import compute_pbo
from py_core.validation.splits import Split
from py_core.validation.walk_forward import run_walk_forward_analysis

_SPEC_STRATEGY_SPEC = "py_core.strategies.spec_strategy:SpecStrategy"

SweepAxis = tuple[str, str]  # (indicator id, field name -- "param", or an operand field name)
SweepSpace = dict[SweepAxis, list[float | int]]


def _default_selection_metric(metrics: BacktestMetrics) -> float:
    return metrics.sharpe_ratio


def generate_spec_variants(base_spec: StrategySpecDoc, sweep: SweepSpace) -> list[StrategySpecDoc]:
    """Cartesian product of every axis in ``sweep``, each producing one variant spec with
    those fields overridden (``base_spec`` itself is never mutated -- frozen dataclasses,
    ``dataclasses.replace()`` throughout).

    ``field_name == "param"`` targets the node's ``window``/``n`` field; any other name must
    be one of the node's existing operand field names (``input``/``left``/``right``/``cond``/
    ``then``/``otherwise``, depending on its operator's shape) and is overridden as a literal
    numeric value (matching how a literal operand is already represented post-parse -- see
    ``schema.py``'s ``OperandValue``).

    Raises:
        ValueError: sweep is empty, references an unknown indicator id, targets ``"param"``
            on a node that has none, or targets an operand field name the node doesn't have.
    """
    if not sweep:
        raise ValueError("sweep 不能为空")

    node_index = {node.id: i for i, node in enumerate(base_spec.indicators)}
    axes = list(sweep.keys())
    for node_id, field_name in axes:
        if node_id not in node_index:
            raise ValueError(f"sweep 引用了不存在的 indicator id: {node_id!r}")
        node = base_spec.indicators[node_index[node_id]]
        if field_name == "param":
            if node.param is None:
                raise ValueError(f"indicator {node_id!r}（op={node.op!r}）没有 param 字段（window/n），不能对它做 sweep")
        elif field_name not in node.operands:
            raise ValueError(f"indicator {node_id!r}（op={node.op!r}）没有字段 {field_name!r}")

    value_lists = [sweep[axis] for axis in axes]
    variants: list[StrategySpecDoc] = []
    for combination in itertools.product(*value_lists):
        indicators = list(base_spec.indicators)
        for (node_id, field_name), value in zip(axes, combination, strict=True):
            i = node_index[node_id]
            node = indicators[i]
            if field_name == "param":
                indicators[i] = dataclasses.replace(node, param=int(value))
            else:
                new_operands = dict(node.operands)
                new_operands[field_name] = float(value)
                indicators[i] = dataclasses.replace(node, operands=new_operands)
        variants.append(dataclasses.replace(base_spec, indicators=tuple(indicators)))
    return variants


def compute_data_digest(records: list[NormalizedOhlcvRecord]) -> str:
    """``sha256:<hex>`` over every record's timestamp + OHLCV values, in order -- changes if
    the actual data changes, not just its date range or record count (§4.5: "保证可复现")."""
    hasher = hashlib.sha256()
    for r in records:
        hasher.update(
            f"{r.event_time_utc.isoformat()}|{r.open_price}|{r.high_price}|{r.low_price}|"
            f"{r.close_price}|{r.volume}\n".encode()
        )
    return f"sha256:{hasher.hexdigest()}"


@dataclass(frozen=True, slots=True)
class ValidationSweepReport:
    winning_spec: StrategySpecDoc  # includes the populated `validation` field
    trials: int
    oos_sharpe: float
    pbo: float
    deflated_sharpe_ratio: float  # extra diagnostic -- see module docstring; not written to TOML ([validation] has no dsr field per §4.5)


def run_validation_sweep(
    config: BacktestConfig,
    records: list[NormalizedOhlcvRecord],
    base_spec: StrategySpecDoc,
    sweep: SweepSpace,
    *,
    walk_forward_splits: list[Split],
    cpcv_n_groups: int,
    purge_bars: int = 0,
    embargo_bars: int = 0,
    selection_metric: Callable[[BacktestMetrics], float] = _default_selection_metric,
) -> ValidationSweepReport:
    """Run the full sweep-then-validate pipeline. See module docstring for the winner-
    selection-vs-oos_sharpe/pbo-computation distinction.

    Args:
        cpcv_n_groups: passed to ``compute_pbo()`` as ``n_groups`` (must be even -- CSCV's
            own requirement, propagated unchanged).

    Raises:
        ValueError: from generate_spec_variants()/run_walk_forward_analysis()/compute_pbo()'s
            own validation.
    """
    variants = generate_spec_variants(base_spec, sweep)
    param_grid: list[dict[str, Any]] = [{"parsed_spec": v} for v in variants]
    trials = len(param_grid)

    wf_report = run_walk_forward_analysis(
        config, records, _SPEC_STRATEGY_SPEC, param_grid, walk_forward_splits, selection_metric=selection_metric
    )
    pbo_report = compute_pbo(
        config,
        records,
        _SPEC_STRATEGY_SPEC,
        param_grid,
        n_groups=cpcv_n_groups,
        purge_bars=purge_bars,
        embargo_bars=embargo_bars,
        selection_metric=selection_metric,
    )
    dsr_result = compute_deflated_sharpe_ratio(wf_report.oos_returns, n_trials=trials)

    df = records_to_dataframe(records)
    annualization_factor = resolve_annualization_factor(config, records)
    best_index: int | None = None
    best_score = -math.inf
    for i, variant in enumerate(variants):
        signal = SpecStrategy(parsed_spec=variant).generate_signals(df)
        result = _run_vectorized_backtest_on_df(config, df, signal, annualization_factor=annualization_factor)
        score = selection_metric(result.metrics)
        if best_index is None or score > best_score:
            best_score = score
            best_index = i
    assert best_index is not None  # variants is non-empty (sweep validated non-empty above)

    validation = ValidationRecord(
        validated_at=datetime.now(UTC).strftime("%Y-%m-%dT%H:%M:%SZ"),
        validated_by="walk_forward+cpcv+pbo",
        data_start_utc=records[0].event_time_utc.strftime("%Y-%m-%dT%H:%M:%SZ"),
        data_end_utc=records[-1].event_time_utc.strftime("%Y-%m-%dT%H:%M:%SZ"),
        data_digest=compute_data_digest(records),
        oos_sharpe=wf_report.oos_metrics.sharpe_ratio,
        pbo=pbo_report.probability_of_backtest_overfitting,
        trials=trials,
    )
    winning_spec = dataclasses.replace(variants[best_index], validation=validation)

    return ValidationSweepReport(
        winning_spec=winning_spec,
        trials=trials,
        oos_sharpe=validation.oos_sharpe,
        pbo=validation.pbo,
        deflated_sharpe_ratio=dsr_result.deflated_sharpe_ratio,
    )


def render_validation_toml_block(validation: ValidationRecord) -> str:
    """Render a ``[validation]`` TOML table matching §4.5's field order/shape exactly."""
    return (
        "[validation]\n"
        f'validated_at   = "{validation.validated_at}"\n'
        f'validated_by   = "{validation.validated_by}"\n'
        f'data_start_utc = "{validation.data_start_utc}"\n'
        f'data_end_utc   = "{validation.data_end_utc}"\n'
        f'data_digest    = "{validation.data_digest}"\n'
        f"oos_sharpe     = {validation.oos_sharpe!r}\n"
        f"pbo            = {validation.pbo!r}\n"
        f"trials         = {validation.trials}\n"
    )


def write_validation_section(toml_text: str, validation: ValidationRecord) -> str:
    """Replace an existing ``[validation]`` table in ``toml_text``, or append a new one if
    absent. Deliberately a string-level operation (not a full TOML AST rewrite -- ``tomllib``
    is read-only, no writer in the stdlib) so every OTHER part of the original file
    (comments, formatting, key order, any table before or after ``[validation]``) survives
    untouched; only the ``[validation]`` table itself changes.
    """
    block = render_validation_toml_block(validation)
    match = re.search(r"^\[validation\](?:\n(?!\[).*)*", toml_text, flags=re.MULTILINE)
    if match:
        return toml_text[: match.start()] + block + toml_text[match.end() :]
    separator = "" if toml_text.endswith("\n") else "\n"
    return toml_text + separator + "\n" + block
