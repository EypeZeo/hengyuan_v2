"""Batch 5, 5c: Python-side drift protection for the cross-language consistency fixtures
``py_core/tools/generate_strategy_spec_fixture.py`` produces.

Re-evaluates the same checked-in ``native/tests/fixtures/strategy_spec/*.golden.json``
fixtures against the *current* ``py_core/indicators/operators.py`` and fails if someone
changed an operator's semantics without regenerating the fixture in the same PR (docs/
STRATEGY_SPEC.md §2.2/§6's rule, restated in the generator script's own docstring).

The C++ side (``native/tests/test_strategy_spec_consistency.cpp``) checks the opposite
direction -- does the C++ streaming evaluator match the fixture -- with zero Python
dependency. This test has zero C++ dependency and runs entirely within ``pytest``/
``ci-python.yml``.
"""

from __future__ import annotations

import json
import math
from pathlib import Path
from typing import Any

import pandas as pd
import pytest

from py_core.strategy_spec import evaluator, schema

_REPO_ROOT = Path(__file__).resolve().parents[2]
_FIXTURE_DIR = _REPO_ROOT / "native" / "tests" / "fixtures" / "strategy_spec"

_FIXTURE_NAMES = ["sma_crossover_btc_1h", "operator_coverage"]


def _load_fixture(name: str) -> dict[str, Any]:
    path = _FIXTURE_DIR / f"{name}.spec.golden.json"
    return json.loads(path.read_text(encoding="utf-8"))


def _load_spec(name: str) -> schema.StrategySpecDoc:
    return schema.load_spec(_FIXTURE_DIR / f"{name}.spec.toml")


@pytest.mark.parametrize("name", _FIXTURE_NAMES)
def test_fixture_node_outputs_match_current_operators(name: str) -> None:
    fixture = _load_fixture(name)
    spec = _load_spec(name)
    df = pd.DataFrame(fixture["bars"])

    node_values = evaluator.evaluate_spec_nodes(spec, df)
    assert set(node_values.keys()) == set(fixture["node_outputs"].keys())

    for node_id, expected in fixture["node_outputs"].items():
        actual = node_values[node_id].to_numpy(dtype=float)
        assert len(actual) == len(expected)
        for i, (a, e) in enumerate(zip(actual, expected, strict=True)):
            if e is None:  # JSON null <-> NaN, this fixture format's own documented convention
                assert math.isnan(a), f"{name}/{node_id} bar {i}: expected NaN, got {a}"
            else:
                assert a == e, f"{name}/{node_id} bar {i}: expected {e}, got {a}"


@pytest.mark.parametrize("name", _FIXTURE_NAMES)
def test_fixture_effective_warmup_matches_current_evaluator(name: str) -> None:
    spec = _load_spec(name)
    fixture = _load_fixture(name)
    assert evaluator.compute_effective_warmup(spec) == fixture["effective_warmup"]
