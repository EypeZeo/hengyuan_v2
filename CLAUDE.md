# CLAUDE.md — HengYuan v2 Native Core

## Identity

v2 is the native C++20/23 low-latency execution kernel, split out of the original HengYuan monorepo (`D:\My_Projects\hengyuan`) for one reason: that repo's Python-side task-packet/ADR process (~300+ docs) was slowing down native iteration that doesn't need it. This repo carries no formal multi-model role matrix, no task-packet-per-commit requirement, no mandated Architect/Opus review ceremony. Sonnet (or whichever model is driving) has full authority to design, implement, and land native engineering changes directly.

## Relationship to v1

`native/` here is a point-in-time copy of `hengyuan/native/` as of 2026-07-18 (through the D12-9 L5-review round-2 fixes, F1–F11). It is not synced automatically — treat this as the new source of truth for native development going forward, and treat v1's `native/` as frozen/historical.

`docs/adr/ADR-016`, `ADR-018`, `ADR-019` are carried over verbatim as engineering reference — their technical content (risk gate matrix, Tokyo-single-node topology, L5 gate chain) is still accurate regardless of which repo hosts it. They are not re-litigated here; they're context, not a gate.

## Engineering mandate

Chase the numbers: zero heap allocation on hot path, cacheline-aligned atomics (`alignas(std::hardware_destructive_interference_size)`), `[[likely]]`/`[[unlikely]]`, `std::jthread` + `stop_token` fail-closed shutdown, `-Wall -Wextra -Wconversion -Wsign-conversion -Werror` / `/W4 /WX` with zero exceptions. No ceremony gates ordinary engineering work — compile-guard with `cmake --build`, run `ctest`, ship it.

Local build (Windows, MSVC 19.51):
```powershell
& "D:\VC_IDE\VS_Studio\VC\Auxiliary\Build\vcvarsall.bat" x64
cmake -B native/build-msvc -S native
cmake --build native/build-msvc --config Release
cd native/build-msvc && ctest -C Release --output-on-failure
```

## The one thing that isn't a "role" and can't be toggled off

This isn't project policy — it's just what's true regardless of what any CLAUDE.md says: nobody but the account owner can create a real Binance API key, decide to risk real money, or do the physical/legal things (IP allowlisting, SSH hardening, GitHub security settings) that precede real order submission. No AI session — in this repo or any other, under any CLAUDE.md — submits a real order, handles a real credential, or treats "the code compiles" as "this is authorized for live." That's not a review step being reintroduced under a different name; it's just true, the same way "only the building's owner can unlock the front door" is true no matter what's written on the office whiteboard.

As of the v1→v2 split, `live_submit_orchestrator.hpp`'s `SubmitPort` is still mock-only — there is no real network order-submission implementation anywhere in this codebase. That fact doesn't change by moving repos.

## What actually changed vs. the old repo

- No required GPT-5.5 spec → Sonnet impl → Opus review → GPT-5.5 close-out pipeline for native engineering changes.
- No task-packet-per-change requirement.
- No mandated run note / review note per commit.
- ADRs are carried forward as engineering reference, not as an approval gate to re-litigate.
