# Native Architecture — HengYuan v2

Single living reference for `native/`, replacing v1's scattered P2-CORE-*/P2-EXEC-*/D12-* task-packet trail. This describes what exists and its gate level — it is not an approval gate itself; see "Live-readiness status" below for what still requires the owner's own action.

## Module map

| Layer | Files | Role |
|---|---|---|
| **Market data (hot path)** | `binance_market_event.hpp`, `spsc_ring.hpp`, `hot_thread.hpp`, `orderbook.hpp`/`.cpp`, `depth_manager.hpp`, `binance_json_parser.hpp`/`.cpp`, `event_recorder.hpp`, `fixed_point.hpp` | Zero-alloc WS ingestion → SPSC ring → order book reconstruction. This is the actual latency-critical path. |
| **Network transport** | `binance_ws_session.hpp` (WS, market data), `binance_rest_snapshot.hpp` (public REST GET, depth snapshot), `transport_policy.hpp` (allowlist/rate-limit/clock-skew/TLS policy for *any* Binance REST call) | `binance_rest_snapshot.hpp` only talks to public, unauthenticated endpoints. No authenticated REST client exists — see SubmitPort below. |
| **Secrets** | `env_parser.hpp` (L1, synthetic), `env_loader.hpp` (L3, real `.env` load + file-permission/anti-symlink/mlock/secure-wipe), `secure_wipe.hpp` | `binance_signer.hpp`'s HMAC-SHA256 is real and verified against Binance's own test vector, but signing without a network path attached is cryptography with nowhere to go yet. |
| **Risk / simulation** | `risk_gate.hpp`, `sim_executor.hpp`, `input_validator.hpp`, `trade_logger.hpp` | Pre-D12 simulation infrastructure (dry-run only). |
| **Order truth chain (D12 series)** | `account_truth.hpp`, `order_lifecycle.hpp`, `exit_safety.hpp`, `audit_trail.hpp`, `dry_run_evidence.hpp`, `live_submit_orchestrator.hpp` | The gate chain gating real order submission. All pure logic — no network, no persistence beyond an in-memory ring. |
| **Liveness / control** | `kill_switch.hpp`, `shm_heartbeat.hpp`, `intent_channel.hpp`, `preflight_gate.hpp` (renamed to `CODE-PREFLIGHT` internally — see naming note below) | Process-liveness watchdog + the 7-item code-layer preflight (distinct from the operational D3-LIVE checklist). |
| **Demos / harnesses** | `src/binance_feed_demo.cpp`, `src/binance_dry_run_demo.cpp`, `src/latency_bench.cpp`, `src/watchdog_daemon.cpp`, `src/live_submit_evidence_harness.cpp` | Operator-facing entry points. `live_submit_evidence_harness` is the newest — runs `orchestrate_submit()` end-to-end for all 4 dry-run paths (mock `SubmitPort`, no network). |

## Gate levels (from ADR-019 D2, reference only — carried over verbatim in `docs/adr/`)

| Activity | Level | Current state in v2 |
|---|---|---|
| Synthetic env parsing | L1 | Implemented, tested |
| Real `.env` credential load | L3 | Implemented, tested — never independently reviewed |
| Signed read-only request (`GET /api/v3/account`) | L4 | **Does not exist** — no authenticated REST client anywhere |
| Signed order submission (`POST /api/v3/order`) | L5 | **Does not exist** — `SubmitPort` is a mock-only dependency-injection point |

## A naming note (fixed 2026-07-18)

`preflight_gate.hpp`'s 7-item code-layer checklist (kill switch, risk gate, depth sync, heartbeat, signer, operator confirm, regression cert) and ADR-018's operational "D3-LIVE 7 项" (no-withdrawal key, account-truth reconciliation, fail-closed on stale data, exposure caps, phone-app drills, geo/service re-verification, SSH hardening) are **two different lists that both gate the L5 transition independently** — they used to share the name "D3-LIVE 7 项" in comments, which invited confusion. The code-layer one is now labeled `CODE-PREFLIGHT` in comments/printed output; the ADR-018 operational list keeps the "D3-LIVE" name since that's its origin. Passing one does not imply the other passed.

## Live-readiness status

**Nothing here is live-ready, and this document does not change that.** Specifically:

- No real `SubmitPort` implementation exists. See `docs/SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` for the design-only spec — implementation requires Level 3 delegation-policy spec approval before any code is written.
- No real Binance testnet or production credentials have ever been used against this code.
- The operator manual-takeover procedure (`docs/NATIVE_EXIT_SAFETY_RUNBOOK.md`) exists as a document; it has not been drilled by a human against a real Binance account.
- The ADR-018 operational D3-LIVE checklist and the code-layer CODE-PREFLIGHT checklist have never both been executed for real and recorded — only exercised via unit tests / synthetic fixtures.
- Real order submission, real credential handling, and the final go/no-go decision remain the account owner's own action, independent of anything in this repo's CLAUDE.md (see that file's boundary section).

## Where things came from (v1 lineage, for archaeology only — not a dependency)

The full D12-1 through D12-9 implementation and review history, including the Opus L5 review findings (F1–F11, all fixed) and the retroactive governance packets (P2-194 through P2-197), live in `hengyuan` (v1) at `docs/task-packets/P2-194..197-*.md` and `docs/review-checklists/P2-197-L5-*.md`. Worth reading once for context on *why* the gate chain is shaped the way it is; not required reading for ordinary native engineering going forward.
