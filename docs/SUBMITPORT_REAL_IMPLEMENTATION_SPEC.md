# Real `SubmitPort` Implementation — L5 POST Adapter Spec (Draft, Not Implemented)

## Status

**Draft, revision 2.** No code from this spec has been written. This is **L5** (per `docs/NATIVE_ARCHITECTURE.md`'s gate table — signed order-*submission*, the highest gate in this repo), and it **depends entirely on `docs/BINANCE_PRIVATE_REST_L4_SPEC.md`** being accepted first: environment binding, signing discipline, server-time sync, reconciliation, symbol registry, and rate-limit accounting all live there now, not here.

### Revision 2 changelog (Architect review, rejected — revision required)

Revision 1 was reviewed via this repo's Architect role and rejected with 5 blocking findings and 6 required additions. This revision addresses all of them:

- **Reconciliation** is no longer excluded — it's L4 §6, referenced throughout this spec's ambiguous-handling paths.
- **Testnet is now reachable** — environment binding (L4 §1) gives testnet its own base host, allowlist, and credential scope, frozen at startup.
- **`SubmitPort`'s ABI gap** (symbol_id + ticks only, no symbol string or decimal formatting) is fixed via the L4 §5 `SymbolRegistryEntry` + lossless tick→decimal formatting.
- **HTTP response mapping** is now exhaustive (§4 below), replacing the 4-row table that left 5xx, truncated bodies, and 200-with-mismatch unhandled.
- **Persistent audit** is now a hard precondition (§6), not a deferred concern.
- P1 additions — query-string signing discipline, timestamp/recvWindow/`-1021` handling, `newOrderRespType=FULL`, full rate-limit header accounting, an honestly-scoped security-reuse boundary — all moved into L4 where they're shared infrastructure, or folded into this spec's §2–§4 where they're L5-specific.
- Terminology fixed: this repo has no `docs/DELEGATION_POLICY.md` (that's a `hengyuan` v1 file, referenced here only when discussing the review *process*, always with the `hengyuan/` prefix). This repo's own gate vocabulary is `docs/NATIVE_ARCHITECTURE.md`'s L1–L5 table; "Level 3" is not used here.

## Goal

Define how `live_submit_orchestrator.hpp`'s `SubmitPort` — currently a function-pointer injection point satisfied only by test mocks — would be backed by a real, authenticated `POST /api/v3/order` call to Binance.

## What already exists and would be reused as-is

| Component | File | Role |
|---|---|---|
| HMAC-SHA256 signing | `binance_signer.hpp` | Signs the exact query-string bytes per L4 §2's discipline. |
| Transport policy | `transport_policy.hpp` | Per-environment allowlist (L4 §1), rate limiting (post-F7/F8 fixes), clock-skew check, error sanitization. |
| Idempotency / order lifecycle | `order_lifecycle.hpp`, `InFlightRegistry` | Generates `newClientOrderId`, guards duplicate submission, models the ambiguous→reconcile state machine that L4 §6 now actually feeds. |
| Symbol registry | L4 §5 (new) | `symbol_id → symbol string + price_scale + qty_scale + rules_version` — required for wire-format construction, doesn't exist in this codebase yet. |
| Reconciliation query | L4 §6 (new) | `GET /api/v3/order`, doesn't exist in this codebase yet. |

## 1. Request construction

```
POST https://{environment.base_host}/api/v3/order
Query string (fixed field order per L4 §2.1, percent-encoded, signed-bytes==sent-bytes):
  symbol={symbol}&side={BUY|SELL}&type=LIMIT&timeInForce=GTC
  &quantity={qty_decimal}&price={price_decimal}&newClientOrderId={coid}
  &newOrderRespType=FULL
  &recvWindow={ms}&timestamp={now_ms}
  &signature={hmac_sha256_hex(query_string_without_signature)}
Header: X-MBX-APIKEY: {api_key}
```

Changes from revision 1:

- `quantity`/`price` are now decimal strings produced by L4 §5's lossless tick→decimal formatting against the `SymbolRegistryEntry` for this `symbol_id`, not raw ticks (Binance's wire format never sees a tick count).
- `newOrderRespType=FULL` is now a fixed, non-optional parameter (§3 below explains why).
- `OrderType` remains frozen to `Limit` (spec 6.8 / ADR-019 D7 M7) — `type=LIMIT` is the only value this ever emits.

## 2. New component: `binance_private_rest.hpp` (not yet written)

```cpp
struct PrivateRestConfig {
    EnvironmentBinding environment;   // from L4 §1 — immutable after init
    std::uint32_t connect_timeout_ms = 5000;
    std::uint32_t read_timeout_ms = 10000;
    // Per-phase deadlines per L4 §8 — resolve/connect/handshake/write/read each
    // get their own budget, not one shared timeout.
};

class BinancePrivateRestClient {
public:
    bool init(std::string_view api_key, BinanceSigner& signer,
              const EnvironmentBinding& environment) noexcept;

    // Blocking. Called only from the L5 runtime thread that owns SubmitPort,
    // never from the hot path.
    SubmitResponse submit_order(
        std::string_view client_order_id,
        std::uint32_t symbol_id, OrderSide side, OrderType type,
        std::int64_t price_ticks, std::int64_t qty_ticks) noexcept;

    // Implements L4 §6 — called by the orchestrator's Ambiguous-handling path,
    // not by this class internally. Exposed here because it shares the same
    // signing/transport plumbing as submit_order().
    ReconcileQueryResult query_order(
        std::string_view symbol, std::string_view orig_client_order_id) noexcept;

private:
    // Boost.Beast SSL stream. Security contract per L4 §8 — hostname
    // verification, per-phase deadlines, response-size cap are tested
    // properties of THIS class, not inherited from binance_rest_snapshot.hpp.
};
```

## 3. Gates checked before every send

1. `transport_policy::validate_policy()` — once at startup, against the bound environment's policy (L4 §1).
2. `transport_policy::check_endpoint()` — resolved host must be in *this environment's* allowlist.
3. Server-time offset freshness (L4 §2.2) — fail closed if TTL-expired and refresh failed.
4. Symbol registry version check (L4 §5) — fail closed on `rules_version` mismatch between pre-trade validation and request construction.
5. Sign via `BinanceSigner::sign()` over the exact bytes to be sent.
6. **Persistent audit write confirmed** (§6) — POST is forbidden if this hasn't durably succeeded.
7. Send via Beast, honoring per-phase deadlines.
8. `transport_policy::check_http_status()` — reject 3xx (no auto-follow).
9. `transport_policy::check_response_size()` — cap response body size.
10. Parse response JSON via simdjson; validate schema (`orderId`, `clientOrderId`, `status` all present) **before** any status-based branching.
11. Parse rate-limit headers (L4 §7) regardless of outcome, feed back into `RequestWeightTracker`.

## 4. Response mapping (exhaustive — replaces revision 1's 4-row table)

| Condition | `SubmitOutcome` | Notes |
|---|---|---|
| HTTP 200, valid JSON, schema present, returned `clientOrderId` == sent COID, `status` ∈ {NEW, PARTIALLY_FILLED, FILLED} | `Accepted` | Routes to the *actual* lifecycle state (Accepted/PartialFill/Filled) via `order_lifecycle.hpp` — `newOrderRespType=FULL` guarantees `fills[]` is present for PARTIALLY_FILLED/FILLED; these are **not** collapsed into a generic "Accepted" and must be written into lifecycle/audit as their real state. |
| HTTP 200, but body fails to parse, schema mismatch, or `clientOrderId` doesn't match sent value | `Ambiguous` | A 200 status code alone is never sufficient — the payload must positively identify *this* order. |
| HTTP 400 with a recognized Binance error code (e.g. `-1013`, `-2010`) | `Rejected` | Order never reached matching-engine acceptance. |
| HTTP 400 with `-1021` | *(handled at L4 §2.2, not here)* | Forces a clock resync; not a terminal outcome for this order — the request is retried only after resync, still respecting no-blind-retry (this is a pre-send correction, not a post-send retry of an ambiguous state). |
| HTTP 403 | `Rejected` | Blocked before reaching the matching engine (WAF/geo); nothing was submitted. |
| HTTP 429 (rate limit) | **`RateLimited`** *(new outcome — requires adding this variant to `SubmitOutcome` in `live_submit_orchestrator.hpp`)* | Request never reached order processing. Read `Retry-After`, freeze **all** sends (not just retries of this order) until it elapses. This `clientOrderId` remains available for a later, consciously-decided resubmission — not an automatic retry. |
| HTTP 418 (IP auto-ban) | `RateLimited` | Same handling as 429, typically longer `Retry-After`. |
| HTTP 5xx (500/502/503/504), including error code `-1007` (matching-engine timeout) | `Ambiguous` | The request may or may not have reached the matching engine — must reconcile via L4 §6. |
| Connect timeout / read timeout | `Timeout` → `Ambiguous` | Unchanged from revision 1. |
| DNS failure / connection reset / TLS handshake failure | `NetworkError` → `Ambiguous` | Unchanged from revision 1. |
| Truncated/incomplete response body (connection dropped mid-read) | `Ambiguous` | Never assume success on partial data. |

**Principle**: default to `Ambiguous` unless a response is unambiguously, schema-validated interpretable as either accepted or cleanly rejected before matching-engine processing.

## 5. Reconciliation wiring

Any `Ambiguous` outcome routes to `order_lifecycle.hpp::determine_reconcile_action()`, which already returns `QueryOrder` (up to `kMaxQueryAttempts` = 3) or `EscalateToOperator`. `QueryOrder` now has a real implementation: `BinancePrivateRestClient::query_order()` (§2), which is L4 §6's `GET /api/v3/order` with the documented Memory→Database-lag retry/backoff. This spec does not change the state machine — it wires a real network call into an action the state machine already models correctly.

## 6. Persistent audit — hard precondition, not deferred

`AuditRingSink` (`audit_trail.hpp`) is explicitly an in-memory, test/first-path sink (F5 fix made it fail-closed *once full*, but it still doesn't survive a process crash). This design **cannot be wired to real network submission** until audit persistence satisfies:

1. `OrderIntentCreated` is durably written (fsync'd file append, or committed DB transaction) and **write success is confirmed** before proceeding.
2. `OrderSubmitted` is durably written and confirmed **before** the POST is actually sent (gate 6 in §3's ordered list).
3. If either durable write fails, the POST **must not** be sent — fail closed, matching `can_submit_order()`'s existing spirit, but enforced against a store that actually survives a crash.

The reason this can't wait: a crash between "POST sent" and "outcome recorded" is exactly the ambiguous-order scenario this whole gate chain exists to handle safely — and it's unrecoverable if the *intent itself* wasn't durably recorded first. Deferring persistent audit "for later" means the highest-risk moment (an in-flight order during a crash) is the one moment this design can't account for.

## 7. What this design deliberately does not do

- Does not implement `binance_private_rest.hpp` — that's the actual implementation, gated on this spec's acceptance plus L4's.
- Does not implement or propose an implementation timeline.
- Does not add auto-detection of environment (testnet vs production) — that remains an explicit, frozen-at-startup operator choice (L4 §1), never inferred.

## Before implementation begins

1. `docs/BINANCE_PRIVATE_REST_L4_SPEC.md` accepted first — this spec is not self-sufficient without it.
2. This spec itself needs Architect acceptance.
3. A real dry-run harness exercising this shape against Binance **testnet** (owner-provided testnet credentials, never production).
4. Persistent audit storage exists and is wired in per §6 — not deferred.
5. Independent review of the actual implementation once written.
6. The owner's own explicit decision to authorize anything beyond testnet — no role in this repo's process grants that on its own.
