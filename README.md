# 衡渊 / HengYuan v2 — Native Core + Research Utilities

Split out of the original HengYuan v1 monorepo on 2026-07-18, to let native engineering iterate
without v1's task-packet/ADR process. See `CLAUDE.md` for why and for the engineering ground rules.

## What's here

- `native/` — C++20/23 low-latency execution kernel: market data parsing, order book, risk gates,
  kill switch, a durable key-rotating audit trail (`DurableControlPlaneSink`), order lifecycle +
  reconciliation, and the live-submit orchestrator (currently mock-only, see below). See
  `docs/NATIVE_ARCHITECTURE.md` for the module map and gate-level status.
- `py_core/` — Python research/backtest utilities: vectorized backtest engine, risk-sizing
  simulator, a pluggable strategy framework (`py_core/strategies/`), and a read-only public-REST
  historical OHLCV fetcher (`py_core/market_data/`, Binance only). See `py_core/README.md`.
- `formal/` — TLA+ specification models verifying crash-recovery invariants for the durable
  control plane. See `formal/README.md` and `docs/SPEC_INVARIANTS.md`.
- `docs/adr/` — architecture decision records, carried over as engineering reference.
- `docs/NATIVE_ARCHITECTURE.md`, `docs/NATIVE_EXIT_SAFETY_RUNBOOK.md` — consolidated native
  reference and the operator manual-takeover runbook.
- `docs/BINANCE_PRIVATE_REST_L4_SPEC.md` + `docs/SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` —
  design-only specs for signed private REST and real order submission. Not implemented; not
  authorized.
- `.github/workflows/` — CI: build+test, sanitizers, spec verification, CodeQL, dependency
  updates.

## Build (native)

```powershell
# initialize your MSVC 64-bit dev environment first, e.g.:
# & "<path-to-your-Visual-Studio-install>\VC\Auxiliary\Build\vcvarsall.bat" x64
cmake -B native/build-msvc -S native
cmake --build native/build-msvc --config Release
cd native/build-msvc && ctest -C Release --output-on-failure
```

## Test (py_core)

```powershell
cd py_core
python -m venv .venv
.venv\Scripts\pip install -r requirements.txt
.venv\Scripts\pip install -e . --no-deps
.venv\Scripts\python -m pytest tests/ -v
```

See `py_core/README.md` for details. Fully independent of v1 — v1 is not a runtime dependency of
v2.

## Live-trading status

`live_submit_orchestrator.hpp`'s `SubmitPort` is mock-only. No real Binance order-submission
network path exists in this codebase, and no real testnet/production credentials have ever been
used against it. See `CLAUDE.md`'s boundary section and `docs/NATIVE_ARCHITECTURE.md`'s
"Live-readiness status" for what real trading would actually require.
