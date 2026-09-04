# Native Architecture — HengYuan v2

Single living reference for `native/`, replacing v1's scattered P2-CORE-*/P2-EXEC-*/D12-* task-packet trail. This describes what exists and its gate level — it is not an approval gate itself; see "Live-readiness status" below for what still requires the owner's own action.

## Module map

| Layer | Files | Role |
|---|---|---|
| **Market data (hot path)** | `binance_market_event.hpp`, `spsc_ring.hpp`, `hot_thread.hpp`, `orderbook.hpp`/`.cpp`, `depth_manager.hpp`, `binance_json_parser.hpp`/`.cpp`, `event_recorder.hpp`, `fixed_point.hpp` | Zero-alloc WS ingestion → SPSC ring → order book reconstruction. This is the actual latency-critical path. |
| **Network transport** | `binance_ws_session.hpp` (WS, market data), `binance_rest_snapshot.hpp` (public REST GET, depth snapshot), `transport_policy.hpp` (allowlist/rate-limit/clock-skew/TLS policy for *any* Binance REST call), `binance_private_rest.hpp` (signed private REST client — `fetch_account`/`query_order`/`submit_order`/`create_listen_key`/`keepalive_listen_key`/`close_listen_key`, all implemented and unit-tested against real success/failure paths), `binance_tls.hpp` (shared TLS/certificate infrastructure — SSL context + SNI config, hostname verification, Windows root-store diagnostics), `binance_listen_key_publisher.hpp`/`binance_user_data_ws_session.hpp`/`binance_user_data_ws_supervisor.hpp` (user-data-stream WS session + reconnect/backoff supervisor), `binance_user_data_event.hpp` (L1, zero-Boost event struct + drain function bridging the WS session to the rest of the order truth chain), `spot_rate_limit_budget.hpp` (per-lane REQUEST_WEIGHT/RAW_REQUESTS/ORDERS budget accounting) | A real, signed, authenticated REST client exists (`binance_private_rest.hpp`) — it is simply not yet wired into any running `native/src/` process with real credentials; see SubmitPort below. |
| **Secrets** | `env_parser.hpp` (L1, synthetic), `env_loader.hpp` (L3, real `.env` load + file-permission/anti-symlink/mlock/secure-wipe), `secure_wipe.hpp`, `binance_signer.hpp`, `binance_environment.hpp`, `binance_query_signing.hpp`, `binance_clock_sync.hpp`, `binance_decimal.hpp` | `binance_signer.hpp`'s HMAC-SHA256 is real and verified against Binance's own test vector; `binance_environment.hpp`/`binance_query_signing.hpp`/`binance_clock_sync.hpp` add environment binding, §2.1 query canonicalization/signing, and §2.2/§7.1.2 clock-offset freshness on top of it; `binance_decimal.hpp` adds fixed-point↔decimal-string codec for wire requests/responses. This network path is now attached (`binance_private_rest.hpp`) — the remaining gap is real credentials never having been loaded/bound in any `native/src/` process (see "Live-readiness status" below). |
| **Risk / simulation** | `risk_gate.hpp`, `sim_executor.hpp`, `input_validator.hpp`, `trade_logger.hpp` | Pre-D12 simulation infrastructure (dry-run only). |
| **Order truth chain (D12 series)** | `account_truth.hpp`, `order_lifecycle.hpp`, `exit_safety.hpp`, `audit_trail.hpp`, `dry_run_evidence.hpp`, `live_submit_orchestrator.hpp`, `order_tracker.hpp` (reconciliation poll loop), `position_truth.hpp` (net-position truth), `order_fill_context.hpp` (dedup-safe cumulative-fill deltas) | The gate chain gating real order submission, plus TODO 1A.4's reconciliation/position-truth additions. Not pure-in-memory-only: Gate 12d/13 durably append via `ctx.durable_audit.append_durable()` — see the Durable persistence / WAL row below. |
| **Durable persistence / WAL** | `durable_audit_sink.hpp`, `durable_log_store.hpp`, `durable_frame_codec.hpp`, `durable_control_plane.hpp` | fsync-forced, MAC-chained write-ahead log the order truth chain's Gate 12d/13 durably append into (`ctx.durable_audit.append_durable()`); backs crash-recovery reconciliation. `durable_control_plane.hpp`'s `DurableRecordType` wire enum is closed/spec-transcribed (see `docs/BINANCE_PRIVATE_REST_L4_SPEC.md` §10) and CI-enforced via `tools/spec_enum_diff.py` — no new enumerator may be added without a NAME_CONFLICT. |
| **Liveness / control** | `kill_switch.hpp`, `shm_heartbeat.hpp`, `intent_channel.hpp`, `preflight_gate.hpp` (renamed to `CODE-PREFLIGHT` internally — see naming note below) | Process-liveness watchdog + the 7-item code-layer preflight (distinct from the operational D3-LIVE checklist). |
| **Demos / harnesses** | `src/binance_feed_demo.cpp`, `src/binance_dry_run_demo.cpp`, `src/latency_bench.cpp`, `src/watchdog_daemon.cpp`, `src/live_submit_evidence_harness.cpp`, `src/binance_connectivity_smoke.cpp`, `src/live_submit_reconcile_harness_demo.cpp` | Operator-facing entry points. `live_submit_evidence_harness` runs `orchestrate_submit()` end-to-end for all 4 dry-run paths (mock `SubmitPort`, no network). `live_submit_reconcile_harness_demo` (TODO 1A.4) is the newest — genuinely real WS thread + reconciliation poll loop + reconnect supervisor running as a process, but `SubmitPort`/`QueryPort` are still mock and the WS session uses a synthetic placeholder listenKey (no real `create_listen_key()` call site exists yet). |

## Gate levels (from ADR-019 D2, reference only — carried over verbatim in `docs/adr/`)

| Activity | Level | Current state in v2 |
|---|---|---|
| Synthetic env parsing | L1 | Implemented, tested |
| Real `.env` credential load | L3 | Implemented, tested — never independently reviewed |
| Signed read-only request (`GET /api/v3/account`) | L4 | **Adapter implemented and unit-tested** (spec rev 72, `binance_private_rest.hpp`) — not yet wired into any running process with real credentials |
| Signed order submission (`POST /api/v3/order`) | L5 | **Adapter implemented and unit-tested** (spec rev 73, `binance_private_rest.hpp`) — `SubmitPort`/`QueryPort` remain mock DI seams in every runnable harness; real adapter not yet wired in |

## A naming note (fixed 2026-07-18)

`preflight_gate.hpp`'s 7-item code-layer checklist (kill switch, risk gate, depth sync, heartbeat, signer, operator confirm, regression cert) and ADR-018's operational "D3-LIVE 7 项" (no-withdrawal key, account-truth reconciliation, fail-closed on stale data, exposure caps, phone-app drills, geo/service re-verification, SSH hardening) are **two different lists that both gate the L5 transition independently** — they used to share the name "D3-LIVE 7 项" in comments, which invited confusion. The code-layer one is now labeled `CODE-PREFLIGHT` in comments/printed output; the ADR-018 operational list keeps the "D3-LIVE" name since that's its origin. Passing one does not imply the other passed.

## Live-readiness status

**Nothing here is live-ready, and this document does not change that.** Development direction
(owner decision, 2026-08): real auto order submission — L4 signed read-only client first, then
the L5 POST adapter. Specifically:

- The L4/L5 adapters (`binance_private_rest.hpp`) are implemented and unit-tested against real
  success/failure response paths, but `live_submit_orchestrator.hpp`'s `SubmitPort`/
  `QueryPort` remain mock DI seams in every runnable harness — nothing in `native/src/` has
  wired the real adapters up and run them as a live process. `docs/BINANCE_PRIVATE_REST_L4_SPEC.md`
  (rev 72, signed read-only foundation: environment binding, signing discipline,
  reconciliation, symbol registry, rate-limit accounting) and
  `docs/SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` (rev 73, the L5 POST adapter, depends on the
  L4 spec) are accepted implementation blueprints. Implementation-level acceptance for each
  is its own fault-injection matrix passing against real code — no v1-style
  Architect/Senior-Reviewer ceremony (see `CLAUDE.md`).
- No real Binance testnet or production credentials have ever been used against this code.
- The operator manual-takeover procedure (`docs/NATIVE_EXIT_SAFETY_RUNBOOK.md`) exists as a document; it has not been drilled by a human against a real Binance account.
- The ADR-018 operational D3-LIVE checklist and the code-layer CODE-PREFLIGHT checklist have never both been executed for real and recorded — only exercised via unit tests / synthetic fixtures.
- Real order submission, real credential handling, and the final go/no-go decision remain the account owner's own action, independent of anything in this repo's CLAUDE.md (see that file's boundary section).

## Where things came from (v1 lineage, for archaeology only — not a dependency)

The full D12-1 through D12-9 implementation and review history, including the Opus L5 review findings (F1–F11, all fixed) and the retroactive governance packets (P2-194 through P2-197), live in `hengyuan` (v1) at `docs/task-packets/P2-194..197-*.md` and `docs/review-checklists/P2-197-L5-*.md`. Worth reading once for context on *why* the gate chain is shaped the way it is; not required reading for ordinary native engineering going forward.
