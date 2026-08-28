# CLAUDE.md — HengYuan v2 Native Core

## Identity

v2 is the native C++20/23 low-latency execution kernel, split out of the original HengYuan monorepo (`D:\My_Projects\hengyuan`) for one reason: that repo's Python-side task-packet/ADR process (~300+ docs) was slowing down native iteration that doesn't need it. This repo carries no formal multi-model role matrix, no task-packet-per-commit requirement, no mandated Architect/Opus review ceremony. Sonnet (or whichever model is driving) has full authority to design, implement, and land native engineering changes directly.

**This file is the single source of engineering rules.** `AGENTS.md` is a one-line pointer to it, so rules written here apply identically to every client (Claude Desktop, OpenCode, Codex, Cursor).

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

## Task execution tiers

Every task is either **Tier A (mechanical)** or **Tier B (design)**. State the tier as your first line of output.

### Tier A — mechanical: spec-faithful porting, test completion, compile fixes

Applies when: no design decisions, no concurrency, no state-machine changes.

1. Zero preamble. Read the target files, then edit directly — no proposals, no explanations.
2. Read only this file + the task's files. Do not read the full specs or `docs/SPEC_INVARIANTS.md`; landed headers are the source of truth for field layouts.
3. Port fields exactly as specified. Do not "improve" types, do not refactor neighbors, do not touch other modules.
4. Validate at the minimum tier: MSVC incremental build + `ctest -R "<related tests>"`.
5. Report in the Output Style format below; 风险 may be empty, 建议下一步 may be "无".

### Tier B — design: concurrency, memory order, state machines, spec↔code arbitration, new components

Applies when: shared state, gate order, lifetime, or a spec/code contradiction changes.

1. First read this file + target headers + the relevant `docs/SPEC_INVARIANTS.md` entries.
2. Output a ≤5-line plan (change surface / invariant impact / validation tier / explicit non-goals); wait for "开始" before writing code.
3. Deliver evidence: a regression test + the sanitizer tier the change needs.
4. Validation tiers by change type:
   - concurrency / shared state → WSL2 `thread` (both negative controls must still fail)
   - memory / lifetime → WSL2 `address`
   - everything else → MSVC + WSL2 `none`
   - integrator before merge → `wsl_verify.sh all` + push CI
5. 风险 in the report must not be empty.

### Both tiers

- One file, one concern per edit. Stay inside the assigned slice.
- Each client works in its own git worktree: `git status` clean at start, commit per task (message carries the slice name), never touch main or another client's worktree.
- Report `file:symbol` only. No code dumps, no tutorials, no "可以考虑"-style advice.

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
```

Dev-loop shortcut (Tier A tasks): steps 2–3 are incremental once configured; use
`ctest -C Release -R "<test-name-regex>"` to run only the tests the change touches. The
full `ctest` run above is the pre-commit tier.

## Local Validation Runbook (WSL2 + GCC, sanitizers)

**MSVC passing does not mean GCC passes, and neither means the sanitizers pass.** CI
(`ci-native.yml`, `ci-native-sanitizers.yml`) builds with GCC-14 on `ubuntu-24.04`, and only
the sanitizer jobs can see memory-safety and data-race bugs at all — MSVC has never once
caught one of those in this repo, and the reverse is also true (GCC's `-Wconversion`/
`-Wmaybe-uninitialized` have flagged things MSVC's `/W4` didn't). Both toolchains must be run
before trusting a change; treat one green build as half a signal, not a full one.

WSL2 (Ubuntu-24.04, matching CI's runner exactly) is the local way to run the GCC side and the
sanitizers MSVC cannot provide (ASan/UBSan; MSVC has ASan but no TSan/UBSan).

One-time setup (interactive — `sudo` needs a password, this cannot be scripted/run headlessly):
```bash
wsl -d Ubuntu-24.04 -- bash -lc "sudo apt-get update && sudo apt-get install -y g++-14 cmake libboost-dev libboost-system-dev libssl-dev ccache ninja-build mold"
```

Then, for every run:
```powershell
# Sync the current working tree (including uncommitted changes) into WSL2's native
# filesystem — building on /mnt/... crosses the 9p protocol and is markedly slower
# for FetchContent's many-small-file simdjson/googletest builds.
wsl -d Ubuntu-24.04 -- bash -lc "bash ~/repos/hengyuan_v2/tools/wsl_sync.sh"

# none = mirrors ci-native.yml (plain GCC-14 Release)
# address = mirrors ci-native-sanitizers.yml's ASan+UBSan job
# thread = mirrors the TSan concurrency job, INCLUDING the negative control
#          (tsan_control_relaxed_ring) that must itself fail with a reported
#          data race — see tools/wsl_verify.sh and formal/README.md's TLA+
#          controls for the same "the control must still fail" discipline.
wsl -d Ubuntu-24.04 -- bash -lc "bash ~/repos/hengyuan_v2/tools/wsl_verify.sh all"
```

**Run frequency — do not run `all` every iteration:**
- dev loop → `none` only
- before commit → `address` (plus `thread` only if the change touches concurrency/shared state)
- before merge → `all`

`tools/wsl_verify.sh` auto-detects ccache/ninja/mold (installed by the one-time setup above)
and turns repeat edit/build/verify loops into mostly cache hits.

**A real finding from standing this up, worth remembering**: on this toolchain (GCC 14.2,
WSL2), TSan silently misses races on multi-word struct copies at `-O1` and even at `-O2`
(RelWithDebInfo's default) — confirmed with standalone repros before touching the real code.
`native/cmake/Sanitizers.cmake` therefore forces `-O0` specifically for `HY_SANITIZER=thread`,
overriding the build type's default optimization. Do not "clean up" that override without
re-running `tsan_control_relaxed_ring` and confirming it still reports a race at whatever
optimization level you switch to — a TSan job that stops detecting races reports the same
"no error" output as one that's actually working.

Also WSL2-specific: TSan can fail outright at startup with `FATAL: ThreadSanitizer: unexpected
memory mapping`, caused by WSL2's default ASLR layout conflicting with TSan's fixed shadow-memory
region. `native/CMakeLists.txt` works around this via `CROSSCOMPILING_EMULATOR` (wrapping every
invocation, including CMake's own build-time `gtest_discover_tests` enumeration, with
`setarch <arch> -R`); `tools/wsl_verify.sh` applies the same wrapper to the TSan control binary,
which isn't ctest-registered. Harmless on runners that don't need it (plain Ubuntu CI).

## Live-trading direction & boundary

**Development direction (owner decision, 2026-08): real auto order submission is the explicit
target.** The two specs are accepted implementation blueprints:

- `docs/BINANCE_PRIVATE_REST_L4_SPEC.md` (rev 72) — signed read-only client + durable
  control-plane store. **Next implementation milestone.**
- `docs/SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` (rev 73) — the L5 POST adapter. Follows and
  depends on L4.

Implementation-level acceptance for each is defined by its own fault-injection matrix passing
against real code, plus this repo's MSVC/WSL2/sanitizer discipline — not a v1-style review
ceremony. Much of L4 §10's ABI surface is already ported into
`native/include/hengyuan/durable_control_plane.hpp` (tracked in `docs/SPEC_INVARIANTS.md`).

**Current honest state:** `live_submit_orchestrator.hpp`'s `SubmitPort` is still a mock
dependency-injection seam; no authenticated REST client exists yet; no real testnet or
production credentials have ever been used against this code.

**What remains true regardless of any file in this repo:** nobody but the account owner can
create a real Binance API key, decide to risk real money, or do the physical/legal things
(IP allowlisting, SSH hardening, GitHub security settings) that precede real order submission.
No AI session submits a real order, handles a real credential, or treats "the code compiles"
as "this is authorized for live." The first live order remains the owner's own action.

## What actually changed vs. the old repo

- No required GPT-5.5 spec → Sonnet impl → Opus review → GPT-5.5 close-out pipeline for native engineering changes.
- No task-packet-per-change requirement.
- No mandated run note / review note per commit.
- ADRs are carried forward as engineering reference only — focus completely on autonomous, aggressive, high-quality development loops.

## Output Style for Claude

Default language for this repo is Chinese, with English technical identifiers preserved. Claude Code should be concise but explicit, skipping generic filler text:

```
完成：[Modified implementation components/headers]
验证：[Exact local compilation/GTest output summary]
风险 / 突破：[Optimizations achieved, latency reductions, or remaining gaps]
建议下一步：[Next targeted task or component]
```
