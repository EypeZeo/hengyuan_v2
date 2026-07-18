# py_core — reusable research/backtest utilities

Ported from `hengyuan/research/backtests/`, `hengyuan/research/risk/`, `hengyuan/app/domain/market/manual_ohlcv.py`, and `hengyuan/app/core/` on 2026-07-18. All internal imports rewritten from `app.domain.*` / `research.*` to `py_core.*`.

## What's here and why it's safe to be here

- `manual_ohlcv.py` — OHLCV data model/validation. Pure dataclasses, stdlib only.
- `backtests/` — vectorized backtest engine, metrics, risk-aware backtest simulation, artifact export, CLI. This is **research/simulation only**: it computes what a strategy *would have done* on historical data. It has no path to a live exchange, no network I/O, and doesn't touch account state.
- `risk/` — a risk-sizing calculator (`RiskCalculator`) used *by the backtest simulation* to size hypothetical positions. This is not the same thing as a live pre-trade risk gate — it never runs against real orders or real account state. Verified zero coupling to `app.domain.execution` / `app.infrastructure` in v1 before porting (`grep` returned nothing).
- `core/` — generic infra: exceptions, logging, pydantic-settings scaffolding. No business logic.

## What was deliberately NOT ported (stays in v1, under governance)

- `app/domain/execution/`, `app/domain/risk/`, `app/domain/portfolio/`, `app/domain/strategy/` — live/production business logic. Per `hengyuan/docs/DELEGATION_POLICY.md` Level 3, this category requires GPT-5.5 spec → Sonnet impl → Opus review → GPT-5.5 close-out. Moving it into v2 (which has no such requirement by design) would strip review from exactly the code that needs it most.
- `app/infrastructure/exchanges/` — live exchange adapters.
- `research/paper_trading/` — the Python-side paper-trading engine. Left in v1 because it's an execution-adjacent simulation of order lifecycle/reconciliation that overlaps conceptually with the native C++ orchestrator's job; porting it here would blur which repo owns "order lifecycle," not simplify anything.

## Setup (independent of v1)

```powershell
cd py_core
python -m venv .venv
.venv\Scripts\pip install -r requirements.txt
.venv\Scripts\pip install -e . --no-deps
```

## Verification

`.venv\Scripts\python -m pytest tests/ -v` — 104/104 passing (copied from v1's existing test suite for these exact modules, imports rewritten, zero test-logic changes). Runs entirely from this repo's own venv — no dependency on `hengyuan/.venv`.
