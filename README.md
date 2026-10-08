# 衡渊 / HengYuan v2 — Native Core + Research Utilities

Split out of the original HengYuan v1 monorepo on 2026-07-18, to let native engineering iterate
without v1's task-packet/ADR process. See `CLAUDE.md` for why and for the engineering ground rules.

## What's here

- `native/` — C++20/23 low-latency execution kernel: market data parsing, order book, risk gates,
  kill switch, a durable key-rotating audit trail (`DurableControlPlaneSink`), order lifecycle +
  reconciliation, a signed private REST client (`binance_private_rest.hpp`, L4/L5 adapters
  implemented and unit-tested), and the live-submit orchestrator (`SubmitPort`/`QueryPort` are
  still mock DI seams in every runnable harness — the real adapters exist but nothing in
  `native/src/` has wired them up with real credentials yet, see below). See
  `docs/NATIVE_ARCHITECTURE.md` for the module map and gate-level status.
- `py_core/` — Python research/backtest utilities: vectorized backtest engine, risk-sizing
  simulator, a pluggable strategy framework (`py_core/strategies/`), and a read-only public-REST
  historical OHLCV fetcher (`py_core/market_data/`, Binance only). See `py_core/README.md`.
- `recorder/` — the D0 market-data recorder: public Binance streams only (depth, trades, mark price,
  liquidations) recorded to sealed, hashed segments on a dedicated host, plus an offline verifier,
  maintenance tooling and its own CI. It is a data-asset line that runs in parallel to the execution
  work and never trades. See `recorder/README.md`, `recorder/MAINTENANCE.md` and
  `recorder/DATA_DICTIONARY.md`.
- `formal/` — TLA+ specification models verifying crash-recovery invariants for the durable
  control plane. See `formal/README.md` and `docs/SPEC_INVARIANTS.md`.
- `tools/` — spec cross-reference and enum-diff checks (run by CI), the WSL2 validation scripts
  (`wsl_sync.sh`, `wsl_verify.sh`), the blueprint site builder and its verifiers
  (`tools/doc_site/`), and `closure_ledger_check.py`, which checks the audit closure ledger.
- `docs/adr/` — architecture decision records, carried over as engineering reference.
- `docs/NATIVE_ARCHITECTURE.md`, `docs/NATIVE_EXIT_SAFETY_RUNBOOK.md` — consolidated native
  reference and the operator manual-takeover runbook.
- `docs/HengYuan_v2_全自动量化实盘系统实施指南与TODOLIST.md` (and its generated `.html` view) — the
  controlled implementation blueprint: stations, tasks and their order, the defect matrix, the
  fault-injection registry and the evidence rules. `docs/AUDIT_CLOSURE_LEDGER.md` is its per-defect
  closure record, checked mechanically by `tools/closure_ledger_check.py`.
- `docs/REQUIRED_CHECKS_RUNBOOK.md` — how the ruleset that protects `master` is set up, verified and
  rolled back. `docs/STRATEGY_SPEC.md` — the strategy TOML format and the research causal gate.
- `docs/BINANCE_PRIVATE_REST_L4_SPEC.md` + `docs/SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` —
  implementation-blueprint specs for signed private REST (L4, rev 72) and real order
  submission (L5, rev 73 — depends on L4). Both adapters are implemented in
  `binance_private_rest.hpp`; next milestone is wiring them into a real running process with
  real credentials, not implementing the adapters themselves.
- `.github/workflows/` — CI: `ci-native.yml` (GCC-14 build + tests), `ci-native-sanitizers.yml`
  (weekly ASan+UBSan, TSan and an ARM64 weak-memory job), `ci-python.yml` (`py_core`),
  `ci-recorder.yml`, `ci-spec-verification.yml` (invariant cross-reference, enum diff, TLA+ models)
  and `main-merge-guard.yml`. `ci-native.yml`, `ci-python.yml`, `ci-recorder.yml` and
  `ci-spec-verification.yml` run on every pull request and end in a `gate` job; `master` is
  protected by the ruleset `master-gate` (pull request, nine required
  checks, up to date with `master`, no bypass). CodeQL is GitHub's default setup and dependency
  updates come from `.github/dependabot.yml`.

## Build (native)

```powershell
# initialize your MSVC 64-bit dev environment first, e.g.:
# & "<path-to-your-Visual-Studio-install>\VC\Auxiliary\Build\vcvarsall.bat" x64
cmake -B native/build-msvc -S native -DCMAKE_BUILD_TYPE=Release -DHY_BUILD_DEMO=ON
cmake --build native/build-msvc --config Release
cd native/build-msvc && ctest -C Release --output-on-failure
```

`-DHY_BUILD_DEMO=ON` matches CI; without it the Binance L4/L5 targets (and everything that needs
Boost or OpenSSL) are not built at all. `CLAUDE.md` has the full validation runbook: MSVC, WSL2/GCC
and the sanitizer tiers.

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

Development direction (owner decision, 2026-08): real auto order submission. The L4 signed
read-only client (`docs/BINANCE_PRIVATE_REST_L4_SPEC.md`, rev 72) and the real L5 `SubmitPort`
POST adapter (`docs/SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md`, rev 73) are both implemented in
`binance_private_rest.hpp` and unit-tested against real success/failure response paths. Both
specs are accepted as implementation blueprints; implementation-level acceptance is their
fault-injection matrices passing against real code.

Current state: the adapters exist, but `live_submit_orchestrator.hpp`'s `SubmitPort`/
`QueryPort` are still mock dependency-injection seams in every runnable harness — nothing in
`native/src/` has wired the real adapters up with real credentials and run them as a live
process yet. `create_listen_key()`/`keepalive_listen_key()` have likewise never been called
outside tests. No real testnet/production credentials have ever been used against this code.
Creating real API keys, configuring IP allowlists, and the go/no-go for the first live order
remain the account owner's own actions. See `CLAUDE.md`'s "Live-trading direction & boundary"
and `docs/NATIVE_ARCHITECTURE.md`'s "Live-readiness status".
