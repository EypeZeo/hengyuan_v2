"""SpecStrategy — wraps a parsed StrategySpec (py_core/strategy_spec/) as a
py_core.strategies.base.Strategy — 批次 3, round 2.

Lives here (inside the trusted `py_core.strategies.` namespace, per `load_strategy()`'s own
`_DEFAULT_ALLOWED_PREFIX`), not inside `py_core.strategy_spec` alongside the parsing/
evaluation engine: the engine (schema.py/evaluator.py) is a distinct concern (DAG parsing
and numerical evaluation) from "one more repo-native Strategy implementation" -- keeping
this thin adapter here means ``load_strategy("py_core.strategies.spec_strategy:SpecStrategy",
spec_path=...)`` works without ``allow_external=True``, exactly like every other built-in
strategy, and a spec plugs directly into everything already built around the Strategy ABC:
``run_vectorized_backtest()``, ``run_walk_forward_analysis()``, ``run_cpcv_analysis()``,
``compute_pbo()``.
"""

from __future__ import annotations

from typing import Any

import pandas as pd

from py_core.strategies.base import Strategy, validate_signal_output
from py_core.strategy_spec import schema
from py_core.strategy_spec.evaluator import evaluate_spec


class SpecStrategy(Strategy):
    """Constructed with exactly one of ``spec_path`` (loads and parses a TOML file) or
    ``parsed_spec`` (an already-parsed ``schema.StrategySpecDoc``, for callers that parsed
    once and want to reuse the result across many instances without re-reading a file each
    time -- e.g. round 3's parameter sweep, which constructs one ``SpecStrategy`` per
    candidate parameter set against the SAME base spec).

    Deliberately NOT named ``spec`` (the more obvious name): ``load_strategy()``'s own first
    positional parameter is itself named ``spec`` (the class-spec string, e.g.
    ``"py_core.strategies.spec_strategy:SpecStrategy"``) -- since ``load_strategy()`` forwards
    every OTHER keyword argument straight through as `**params` to this constructor, a
    ``spec=`` here would collide with that parameter the moment a caller (round 3's sweep)
    passes a spec through `load_strategy()`'s param_grid mechanism, raising "got multiple
    values for argument 'spec'". Caught directly via that exact call path, not guessed."""

    def __init__(self, *, spec_path: str | None = None, parsed_spec: schema.StrategySpecDoc | None = None) -> None:
        if (spec_path is None) == (parsed_spec is None):
            raise ValueError("必须且只能传 spec_path 或 parsed_spec 二者之一")
        self.spec = schema.load_spec(spec_path) if spec_path is not None else parsed_spec

    def generate_signals(self, df: pd.DataFrame) -> pd.Series[Any]:
        signal = evaluate_spec(self.spec, df)
        validate_signal_output(signal, df)
        return signal
