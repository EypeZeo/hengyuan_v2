"""StrategySpec — declarative strategy DAG, Python-side evaluator — 批次 3, round 2.

Loads and evaluates the TOML format `docs/STRATEGY_SPEC.md` defines. See `schema.py` for
parsing/structural validation and `evaluator.py` for turning a parsed spec into a signal
Series; `strategy.py` wraps both into a `py_core.strategies.base.Strategy` so a spec plugs
directly into everything that already consumes one (backtests, walk-forward, CPCV, PBO).
"""

from __future__ import annotations
