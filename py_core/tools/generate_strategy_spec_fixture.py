"""Batch 5, 5c: generates the checked-in cross-language consistency golden fixtures under
``native/tests/fixtures/strategy_spec/``.

Run manually (``python -m py_core.tools.generate_strategy_spec_fixture``) whenever an
operator in ``py_core/indicators/operators.py`` changes -- the PR that changes an operator's
semantics must regenerate and commit the updated fixture(s) in the same PR (docs/
STRATEGY_SPEC.md §2.2/§6; ``native/include/hengyuan/strategy_spec_operators.hpp``'s own header
comment states the same rule from the C++ side). This is deliberately **not** a pytest test
(``py_core/pyproject.toml``'s ``testpaths = ["tests"]`` already excludes ``tools/``) -- fixture
regeneration is an offline, human-triggered step, not something CI runs implicitly. The
drift-protection leg lives in ``py_core/tests/test_strategy_spec_fixture_consistency.py``
instead: it re-evaluates these same checked-in fixtures against the current
``py_core/indicators/operators.py`` and fails if someone changed an operator without
regenerating.

Fixture format (JSON): ``{"bars": [{"open":...,"high":...,"low":...,"close":...,
"volume":...}, ...], "node_outputs": {"<node_id>": [v0, v1, ...], ...},
"effective_warmup": N}``. NaN has no native JSON token -- every NaN is written as ``null``,
and both the C++ (``native/tests/test_strategy_spec_consistency.cpp``) and Python
(``test_strategy_spec_fixture_consistency.py``) readers translate ``null <-> NaN`` explicitly
rather than relying on either language's JSON library's own non-standard NaN handling.
``json.dumps(..., allow_nan=False)`` below is a safety net: it raises if any NaN slipped
through un-translated instead of silently emitting the non-standard ``NaN`` token.
"""

from __future__ import annotations

import json
import math
from pathlib import Path

import numpy as np
import pandas as pd

from py_core.strategy_spec import evaluator, schema

_REPO_ROOT = Path(__file__).resolve().parents[2]
_FIXTURE_DIR = _REPO_ROOT / "native" / "tests" / "fixtures" / "strategy_spec"


def _synthetic_ohlcv(n: int, *, seed: int) -> pd.DataFrame:
    """Same seeded geometric-random-walk shape as py_core/tests/test_strategy_spec_evaluator.py's
    own ``_ohlcv_df()`` helper -- deliberately reused, not reinvented, so this fixture's inputs
    are drawn from the same distribution the Python test suite already trusts."""
    rng = np.random.default_rng(seed)
    prices = 100.0 * np.exp(np.cumsum(rng.normal(0.0, 0.01, n)))
    idx = pd.date_range("2024-01-01", periods=n, freq="h")
    return pd.DataFrame(
        {
            "open": prices,
            "high": prices * 1.001,
            "low": prices * 0.999,
            "close": prices,
            "volume": 1000.0,
        },
        index=idx,
    )


def _nan_to_none(value: float) -> float | None:
    return None if math.isnan(value) else float(value)


def _generate_one(spec_path: Path, *, n_bars: int, seed: int) -> Path:
    spec = schema.load_spec(spec_path)
    df = _synthetic_ohlcv(n_bars, seed=seed)
    node_values = evaluator.evaluate_spec_nodes(spec, df)
    effective_warmup = evaluator.compute_effective_warmup(spec)

    fixture = {
        "bars": [
            {
                "open": float(row.open),
                "high": float(row.high),
                "low": float(row.low),
                "close": float(row.close),
                "volume": float(row.volume),
            }
            for row in df.itertuples()
        ],
        "node_outputs": {
            node_id: [_nan_to_none(v) for v in series.to_numpy(dtype=np.float64)]
            for node_id, series in node_values.items()
        },
        "effective_warmup": effective_warmup,
    }

    out_path = spec_path.with_suffix(".golden.json")
    out_path.write_text(json.dumps(fixture, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    print(f"wrote {out_path.relative_to(_REPO_ROOT)} ({n_bars} bars, {len(node_values)} nodes)")
    return out_path


def main() -> None:
    # Fixture 1: docs/STRATEGY_SPEC.md §7's worked example verbatim -- exercises sma/rsi/lt/
    # gt/and and the warm-up boundary the doc itself names (bars 49/50, effective_warmup=50).
    # >=120 bars so there's real post-warm-up signal activity to observe, not just the
    # boundary itself.
    _generate_one(_FIXTURE_DIR / "sma_crossover_btc_1h.spec.toml", n_bars=120, seed=1)

    # Fixture 2: every operator §7's example doesn't touch -- ema, stddev, rolling_max/min,
    # roc, lag, div (incl. a bar engineered to hit the zero-denominator NaN path via lag's own
    # delayed value coinciding with 0 is impractical with this random-walk generator, so this
    # covers the NaN-propagation shape instead), crosses_above/crosses_below (one with a
    # literal operand, per the verified spec<->implementation arbitration -- Batch 5, 5a), not,
    # if_then_else, and a scaled-mode [signal].
    _generate_one(_FIXTURE_DIR / "operator_coverage.spec.toml", n_bars=80, seed=2)


if __name__ == "__main__":
    main()
