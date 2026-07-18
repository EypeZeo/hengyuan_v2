# 衡渊 / HengYuan v2 — Native Core + Research Utilities

Split out of the original HengYuan monorepo (`D:\My_Projects\hengyuan`) on 2026-07-18. See `CLAUDE.md` for why and for the engineering ground rules.

## What's here

- `native/` — the full C++20/23 low-latency execution kernel (market data parsing, order book, risk gates, kill switch, audit trail, order lifecycle, and the D12-9 live-submit orchestrator). See `docs/NATIVE_ARCHITECTURE.md` for the module map and current gate-level status.
- `py_core/` — reusable Python research/backtest utilities (vectorized backtest engine, risk-sizing simulator, OHLCV data model). See `py_core/README.md` for exactly what was ported and why the boundary is where it is (no live execution, no risk gate, no exchange adapter code came along).
- `docs/adr/` — ADR-016/018/019, carried over verbatim as engineering reference.
- `docs/NATIVE_ARCHITECTURE.md`, `docs/NATIVE_EXIT_SAFETY_RUNBOOK.md`, `docs/SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` — consolidated native reference, operator manual-takeover runbook, and the design-only spec for a real order-submission client (not implemented — needs Level 3 spec approval first).
- `.github/workflows/` — CI Native (build+test), CodeQL, main-merge-guard, Dependabot (github-actions ecosystem only), all ported from v1 and re-tuned for the 2000 min/month private-repo Actions budget (narrower Boost install, build-tree caching, timeouts).

## Build (native)

```powershell
& "D:\VC_IDE\VS_Studio\VC\Auxiliary\Build\vcvarsall.bat" x64
cmake -B native/build-msvc -S native
cmake --build native/build-msvc --config Release
cd native/build-msvc && ctest -C Release --output-on-failure
```

## Test (py_core)

```powershell
# using v1's venv until v2 has its own
D:\My_Projects\hengyuan\.venv\Scripts\python.exe -m pytest py_core/tests/ -v
```

## Live-trading status

Unchanged by this migration and by everything ported since: `live_submit_orchestrator.hpp`'s `SubmitPort` is mock-only. No real Binance order-submission network path exists in this codebase. See `CLAUDE.md`'s boundary section and `docs/NATIVE_ARCHITECTURE.md`'s "Live-readiness status" for what that actually requires.
