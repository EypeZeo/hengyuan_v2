# 衡渊 / HengYuan v2 — Native Core

Split out of the original HengYuan monorepo on 2026-07-18. See `CLAUDE.md` for why and for the engineering ground rules.

## What's here

- `native/` — the full C++20/23 low-latency execution kernel (market data parsing, order book, risk gates, kill switch, audit trail, order lifecycle, and the D12-9 live-submit orchestrator). Copied as-is from `hengyuan/native/` at the point of the split, including all GTest suites.
- `docs/adr/` — the three ADRs that describe native/execution-plane decisions (ADR-016 risk gate matrix, ADR-018 Binance-first Tokyo topology, ADR-019 L5 readiness gates), carried over as reference.

## What's not here (yet)

Reusable Python components (strategy backtest utilities, the FastAPI shell, etc.) were not carried over in this first pass — that selection needs a separate look at what in `hengyuan/app/` is actually worth porting versus what's Phase-2 business logic that belongs in the original repo. Same for a consolidated native architecture doc replacing the old repo's task-packet sprawl — not written yet.

## Build

```powershell
& "D:\VC_IDE\VS_Studio\VC\Auxiliary\Build\vcvarsall.bat" x64
cmake -B native/build-msvc -S native
cmake --build native/build-msvc --config Release
cd native/build-msvc && ctest -C Release --output-on-failure
```

## Live-trading status

Unchanged by this migration: `live_submit_orchestrator.hpp`'s `SubmitPort` is mock-only. No real Binance order-submission network path exists in this codebase.
