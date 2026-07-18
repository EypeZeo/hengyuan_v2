- # AGENTS.md — HengYuan v2 Native Core

  ## Identity

  v2 is the native C++20/23 low-latency execution kernel, split out of the original HengYuan monorepo (`D:\My_Projects\hengyuan`) for one reason: that repo's Python-side task-packet/ADR process (~300+ docs) was slowing down native iteration that doesn't need it. This repo carries no formal multi-model role matrix, no task-packet-per-commit requirement, no mandated Architect/Opus review ceremony. Sonnet (or whichever model is driving) has full authority to design, implement, and land native engineering changes directly.

  ## Relationship to v1

  `native/` here is a point-in-time copy of `hengyuan/native/` as of 2026-07-18 (through the D12-9 L5-review round-2 fixes, F1–F11). It is not synced automatically — treat this as the new source of truth for native development going forward, and treat v1's `native/` as frozen/historical.

  `docs/adr/ADR-016`, `ADR-018`, `ADR-019` are carried over verbatim as engineering reference — their technical content (risk gate matrix, Tokyo-single-node topology, L5 gate chain) is still accurate regardless of which repo hosts it. They are not re-litigated here; they're context, not a gate.

  ## Engineering Mandate & Performance Discipline

  Chase the numbers. Every line of code written must satisfy the strict requirements of sub-microsecond crypto trading:

  1. **Zero Heap Allocation**: Absolute zero dynamic memory allocations (`new`, `malloc`, `std::string` concatenation) on the Hot Path. Use `std::span<char>` and `std::string_view` for zero-copy memory views.
  2. **False Sharing Prevention**: Multi-thread/multi-process atomic control blocks and coordinator flags must be cacheline-aligned via `alignas(std::hardware_destructive_interference_size)`.
  3. **Branch Prediction & Compile-Time Delegation**: Maximize the use of `constexpr`/`consteval` to push logic to compile-time. Embed C++20 `[[likely]]` and `[[unlikely]]` attributes to guide CPU execution pipelines straight.
  4. **Busy-Waiting & Thread Pinning**: Lock-free SPSC queues must use busy-waiting active polling loops with hardware hint `_mm_pause()` to eliminate OS context-switch latency. Support processor affinity masking (Core Pinning) for strategy threads.
  5. **Arithmetic Safety**: Enforce strong typing. Zero tolerance for implicit numeric conversions. Code must natively prevent integer overflows under arithmetic operations on position matching and PnL scaling.

  ## Local Validation Runbook (Windows + MSVC)

  Local environment has MSVC 19.51 available via `vcvarsall.bat`. To validate any C++ change locally before committing, always utilize this exact sequence:

  ```powershell
  # 1. Initialize MSVC 64-bit developer environment
  & "D:\VC_IDE\VS_Studio\VC\Auxiliary\Build\vcvarsall.bat" x64
  
  # 2. Configure with aggressive optimization and hardware vectorization flags
  # MSVC: /O2 /Ot /Oi /arch:AVX2 | GCC: -O3 -march=native -ffast-math
  cmake -B native/build-msvc -S native -DCMAKE_BUILD_TYPE=Release
  
  # 3. Build with zero-warning threshold (-Wall -Wextra -Wconversion enforced)
  cmake --build native/build-msvc --config Release
  
  # 4. Execute unit test harness
  cd native/build-msvc && ctest -C Release --output-on-failure

## The one thing that isn't a "role" and can't be toggled off

This isn't project policy — it's just what's true regardless of what any AGENTS.md says: nobody but the account owner can create a real Binance API key, decide to risk real money, or do the physical/legal things (IP allowlisting, SSH hardening, GitHub security settings) that precede real order submission. No AI session — in this repo or any other, under any AGENTS.md — submits a real order, handles a real credential, or treats "the code compiles" as "this is authorized for live." That's not a review step being reintroduced under a different name; it's just true, the same way "only the building's owner can unlock the front door" is true no matter what's written on the office whiteboard.

As of the v1→v2 split, `live_submit_orchestrator.hpp`'s `SubmitPort` is still mock-only — there is no real network order-submission implementation anywhere in this codebase. That fact doesn't change by moving repos.

## What actually changed vs. the old repo

- No required GPT-5.5 spec → Sonnet impl → Opus review → GPT-5.5 close-out pipeline for native engineering changes.
- No task-packet-per-change requirement.
- No mandated run note / review note per commit.
- ADRs are carried forward as engineering reference only — focus completely on autonomous, aggressive, high-quality development loops.

## Output Style for Codex

Default language for this repo is Chinese, with English technical identifiers preserved. Codex should be concise but explicit, skipping generic filler text:

```
完成：[Modified implementation components/headers]
验证：[Exact local compilation/GTest output summary]
风险 / 突破：[Optimizations achieved, latency reductions, or remaining gaps]
建议下一步：[Next targeted task or component]
```
