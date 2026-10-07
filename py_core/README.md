# py_core — reusable research/backtest utilities

Ported from `hengyuan/research/backtests/`, `hengyuan/research/risk/`, and `hengyuan/app/domain/market/manual_ohlcv.py` on 2026-07-18. All internal imports rewritten from `app.domain.*` / `research.*` to `py_core.*`. (`hengyuan/app/core/` was also ported that day as `core/` but removed on 2026-08-29 — AUDIT PYDEAD-CORE-046: zero inbound imports anywhere in this repo since porting, and its two `structlog`/`pydantic_settings` dependencies were never declared in `requirements.txt`/`pyproject.toml`, so it would have crashed with `ModuleNotFoundError` the moment anything actually tried to import it.)

## What's here and why it's safe to be here

- `manual_ohlcv.py` — OHLCV data model/validation. Pure dataclasses, stdlib only.
- `backtests/` — vectorized backtest engine, metrics, risk-aware backtest simulation, artifact export, CLI. This is **research/simulation only**: it computes what a strategy *would have done* on historical data. It has no path to a live exchange, no network I/O, and doesn't touch account state.
- `risk/` — a risk-sizing calculator (`RiskCalculator`) used *by the backtest simulation* to size hypothetical positions. This is not the same thing as a live pre-trade risk gate — it never runs against real orders or real account state. Verified zero coupling to `app.domain.execution` / `app.infrastructure` in v1 before porting (`grep` returned nothing).

## What was deliberately NOT ported (stays in v1, under governance)

- `app/domain/execution/`, `app/domain/risk/`, `app/domain/portfolio/`, `app/domain/strategy/` — live/production business logic. Per `hengyuan/docs/DELEGATION_POLICY.md` Level 3, this category requires GPT-5.5 spec → Sonnet impl → Opus review → GPT-5.5 close-out. Moving it into v2 (which has no such requirement by design) would strip review from exactly the code that needs it most.
- `app/infrastructure/exchanges/` — live exchange adapters.
- `research/paper_trading/` — the Python-side paper-trading engine. Left in v1 because it's an execution-adjacent simulation of order lifecycle/reconciliation that overlaps conceptually with the native C++ orchestrator's job; porting it here would blur which repo owns "order lifecycle," not simplify anything.

## Causal gate (SAFE-05)

Every production path that turns a strategy into signals — the backtest CLI (`run`, `run-risk-aware`), `run_walk_forward_analysis`, `run_cpcv_analysis`, `compute_pbo` and `run_validation_sweep` — goes through `strategies.base.checked_signals()`. It validates the output shape and then runs a prefix-truncation differential: at up to `--causal-points` (default 8) positions after the warm-up it recomputes the signal from the history prefix only and requires it to equal the full-data signal exactly. A mismatch raises `LookaheadBiasError` and nothing is backtested. An AST test pins that no other production module calls `generate_signals()`.

What the report says: `validation_report.json` carries `causal_check` (`prefix_differential` for strategy signals that passed the gate, `index_only` for precomputed signals, `not_run` only for hand-built reports), `causal_points` and `causal_context` (sampled positions, bar count, data digest, and three reserved empty slots for the point-in-time quadruple, the experiment id and the blind-set hash that D1 will fill). The engines check the **raw** signal index *before* reindexing: duplicate timestamps and timestamps outside the OHLCV index (a shifted or time-zone-mismatched index) are rejected.

**Coverage boundary — what this does not prove.** The differential only has force at the sampled bars. It does not prove that a hand-written strategy has no hidden data dependency and says nothing about unsampled bars; a signal file (`--signals`) can only get index and timestamp checks and is reported as `index_only`, never as a causal pass. The remaining risk goes to the experiment audit (D1). `lag.n < 1` in a StrategySpec is rejected at parse time (`LookaheadBiasError`, also a `StrategySpecError`).

## Setup (independent of v1)

```powershell
cd py_core
python -m venv .venv
.venv\Scripts\pip install -r requirements.txt
.venv\Scripts\pip install -e . --no-deps
```

## Verification

`.venv\Scripts\python -m pytest tests/ -v` — all tests passing. The original 2026-07-18 port
carried over v1's existing suite for these exact modules (imports rewritten, zero
test-logic changes) at 104/104; the suite has grown substantially since (new modules,
batch rounds) and the count is no longer meaningful to hardcode here (AUDIT
PYDOC-STALE-048 — a stale count reads as more authoritative than a passing `pytest`
run, not less, so this file no longer states one). Run the command above for the
current number. Runs entirely from this repo's own venv — no dependency on
`hengyuan/.venv`.
