# Real `SubmitPort` Implementation — L5 POST Adapter Spec (Draft, Not Implemented)

## Status

**Draft, revision 4.** No code from this spec has been written. This is **L5** (per `docs/NATIVE_ARCHITECTURE.md`'s gate table — signed order-*submission*, the highest gate in this repo), and it **depends entirely on `docs/BINANCE_PRIVATE_REST_L4_SPEC.md`** being accepted first: environment binding, signing discipline, server-time sync, reconciliation, symbol registry, and rate-limit accounting all live there now, not here.

### Revision 2 changelog (Architect review round 1, rejected)

Revision 1 was reviewed and rejected with 5 blocking findings and 6 required additions:

- **Reconciliation** is no longer excluded — moved to L4 §6.
- **Testnet is now reachable** — environment binding (L4 §1), frozen at startup.
- **`SubmitPort`'s ABI gap** (symbol_id + ticks only) — addressed via L4 §5's symbol registry + lossless formatting.
- **HTTP response mapping** made exhaustive (§4 below).
- **Persistent audit** named as a hard precondition.
- P1 additions (signing discipline, timestamp/recvWindow/`-1021`, `newOrderRespType=FULL`, rate-limit headers, honest security-reuse scoping) — moved into L4 or folded into §2–§4.
- Terminology fixed to this repo's own `docs/NATIVE_ARCHITECTURE.md` L1–L5 vocabulary.

### Revision 3 changelog (Architect review round 2, rejected — revision required)

Round 2 found that round 2's fixes were correct in *direction* but stopped at prose — the ABI, the audit interface, and the environment-exclusivity/rate-limit mechanisms weren't actually concrete enough to review as engineering artifacts. Four more P0 findings and four P1s, all fixed in this revision:

- **P0**: L4's reconciliation query only defined retry semantics for `-2013`; every other failure mode of the query itself (5xx, `-1007`, `429`/`418`, network failure, schema mismatch) was undefined. Fixed in L4 §6 — collapsed into one `Inconclusive` outcome, never a "confirmed absent" conclusion.
- **P0**: This spec's new `RateLimited` outcome and the `Accepted`/`PartialFill`/`Filled` distinction had no path into the actual ABI — `SubmitOutcome` still had 4 values, `SubmitResponse` carried no fill data. Fixed in §4.1 below with the actual enum/struct additions and the orchestrator routing they require.
- **P0**: `rules_version` fail-closed was asserted in prose with no data model. Fixed in L4 §5 — `SymbolRules` extended with the field, `validate_pre_trade()`'s signature carries it out, `OrchestratorContext` carries it forward, `submit_order()` takes it as a parameter and compares it.
- **P0**: Persistent audit was named as a precondition with no interface, no ACK contract, no recovery path, and the current orchestrator code was found to literally ignore every `append()` return value. Fixed in §6 below with a `DurableAuditSink` interface and the actual gate-check code path.
- **P1**: L4's canonical alphabetical query-string order and this spec's own request example disagreed. Fixed in §1 below.
- **P1**: `FULL` response validation used `fills[]` presence as the branching signal; fixed to use `status` with full schema/field validation in §4.2.
- **P1**: `EnvironmentBinding` was a public aggregate, not actually exclusive by construction. Fixed in L4 §1 — private constructor, factory-only.
- **P1**: `RequestWeightTracker` had no correction-from-header API, no order-count bucket, no freeze API. Fixed in L4 §7.

### Revision 4 changelog (Architect review round 3, rejected — revision required)

Round 3 found two genuine safety bugs (not just missing detail) plus two more incomplete-wiring findings. Verified against actual code before fixing, not just against the review's description:

- **P0 (self-contradiction)**: `RateLimited` was specified as "never reached the matching engine, no `Submitting` transition, release in-flight" — but a direct read of the orchestrator's gate ordering shows `InFlightRegistry` registration and the `Submitting` transition **already happen before `SubmitPort::call()` is invoked** (Gate 12b/12c, then the transition, then the actual call). By the time any HTTP response — including 429/418 — comes back, the order is already `Submitting` and already in-flight; "never reached the engine" cannot be asserted about a response that only exists because a request was sent. Binance's own docs don't promise 429/418 means pre-matching-engine rejection either. Fixed (§4.3/§4.4): a post-send `RateLimited` response now routes through `Ambiguous` like any other network-level uncertainty, reconciled via L4 §6 — the only thing distinct about it is reading `Retry-After` to freeze future sends. The genuinely safe "never touched the network" case was already fully handled by the *existing*, unmodified pre-flight `RateLimitExhausted` gate (Gate 8, `can_send()`) — that path needed no new outcome value at all; conflating it with a post-send outcome was the error.
- **P0 (safety bug carried from L4)**: see `BINANCE_PRIVATE_REST_L4_SPEC.md`'s revision 3 changelog — `Found` no longer forces `Reconciled` (terminal); this spec's §4.3 routing table is updated to match.
- **P0**: `rules_version` reached `BinancePrivateRestClient::submit_order()` but never reached `SubmitPort::SubmitFn`/`call()` — the actual injection-point ABI the orchestrator calls — so the value pre-trade validated against could never actually arrive at an implementation sitting behind `SubmitPort`. Fixed in §2 below: `SubmitFn`'s signature gains the parameter, matching `submit_order()`.
- **P1**: "recognized Binance error code (e.g. `-1013`, `-2010`)" wasn't an exhaustive definition — anything not on an explicit list could be silently miscategorized. Fixed in §4.4: a bounded allowlist of codes confirmed pre-matching-engine, everything else defaults to `Ambiguous`.
- **P1**: `ctx.durable_audit` was used in §6.2's pseudocode without being declared as a required field anywhere, and no null-pointer behavior was defined. Fixed in §6.2: added to the proposed `OrchestratorContext` extension, with explicit fail-closed behavior on null (matching the existing pattern for `ctx.audit`).

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
Query string — canonical alphabetical field order per L4 §2.1 (this is the
ONE order both L4 and L5 use; no other ordering is valid anywhere in this
codebase), percent-encoded, signed-bytes==sent-bytes:
  newClientOrderId={coid}&newOrderRespType=FULL&price={price_decimal}
  &quantity={qty_decimal}&recvWindow={ms}&side={BUY|SELL}&symbol={symbol}
  &timeInForce=GTC&timestamp={now_ms}&type=LIMIT
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

    // Current registry version this client will check submit_order()'s
    // expected_rules_version against (L4 §5.3). Updated only by an explicit,
    // operator-triggered refresh — never silently mid-session.
    std::uint32_t current_rules_version() const noexcept;

    // Blocking. Called only from the L5 runtime thread that owns SubmitPort,
    // never from the hot path. Signature matches SubmitPort::SubmitFn (§2.1
    // below) parameter-for-parameter — this is what the function pointer
    // ultimately calls into, so the two signatures must never drift apart.
    SubmitResponse submit_order(
        std::string_view client_order_id,
        std::uint32_t symbol_id, OrderSide side, OrderType type,
        std::int64_t price_ticks, std::int64_t qty_ticks,
        std::uint32_t expected_rules_version) noexcept;  // L4 §5.3 — fail closed
                                                           // (StaleRulesVersion) on mismatch
                                                           // against current_rules_version(),
                                                           // before any request is formatted

    // Implements L4 §6 — called by the orchestrator's Ambiguous-handling path,
    // not by this class internally. Exposed here because it shares the same
    // signing/transport plumbing as submit_order(). Returns Found or
    // Inconclusive per L4 §6.1 — never a "confirmed absent" result.
    ReconcileQueryResult query_order(
        std::string_view symbol, std::string_view orig_client_order_id) noexcept;

private:
    // Boost.Beast SSL stream. Security contract per L4 §8 — hostname
    // verification, per-phase deadlines, response-size cap are tested
    // properties of THIS class, not inherited from binance_rest_snapshot.hpp.
};
```

### 2.1 `SubmitPort` ABI change — fixes round-3 P0 (`rules_version` stopped one layer short of where it needed to go)

Round 3's fixes threaded `expected_rules_version` into `submit_order()` (§2 above), but that function is only ever *called from* `SubmitPort` — the actual injection-point ABI in `live_submit_orchestrator.hpp` — and that ABI was never updated to carry the parameter. A real implementation sitting behind `SubmitPort::fn` would receive no way to know which registry version pre-trade validation used, making §2's whole fail-closed check unreachable in practice.

```cpp
// live_submit_orchestrator.hpp — SubmitPort::SubmitFn and call() both gain
// the parameter, matching submit_order()'s signature exactly:
struct SubmitPort {
    using SubmitFn = SubmitResponse(*)(
        const char* client_order_id,
        std::uint32_t symbol_id,
        OrderSide side,
        OrderType type,
        std::int64_t price_ticks,
        std::int64_t qty_ticks,
        std::uint32_t expected_rules_version,  // NEW — threads L4 §5.3's version through
        void* user_data);

    SubmitFn fn{nullptr};
    void* user_data{nullptr};

    SubmitResponse call(const char* coid, std::uint32_t sym,
                        OrderSide side, OrderType type,
                        std::int64_t price, std::int64_t qty,
                        std::uint32_t expected_rules_version) const noexcept {  // NEW param
        if (!fn) return {SubmitOutcome::NetworkError, 0, -1};
        return fn(coid, sym, side, type, price, qty, expected_rules_version, user_data);
    }

    bool is_valid() const noexcept { return fn != nullptr; }
};
```

The orchestrator's actual call site (currently `ctx.submit_port.call(coid.id, ctx.symbol_id, ctx.side, ctx.order_type, ctx.price_ticks, ctx.qty_ticks)`) passes `ctx.pre_trade_rules_version` (L4 §5.3's `OrchestratorContext` field) as the new argument — this is the concrete, unbroken chain: `validate_pre_trade()` → `OrchestratorContext::pre_trade_rules_version` → `SubmitPort::call()` → `SubmitFn` → `BinancePrivateRestClient::submit_order()`, with no layer silently dropping the value.

## 3. Gates checked before every send

Ordering matches §6.2's actual code shape — the durable audit check is two separate gates (intent, then submitted), not one:

1. `transport_policy::validate_policy()` — once at startup, against the bound environment's policy (L4 §1).
2. `transport_policy::check_endpoint()` — resolved host must be in *this environment's* allowlist.
3. Server-time offset freshness (L4 §2.2) — fail closed if TTL-expired and refresh failed.
4. Symbol registry version check (L4 §5.3) — `expected_rules_version` vs `current_rules_version()`; fail closed (`StaleRulesVersion`) on mismatch, before any request is formatted.
5. **`DurableAuditSink::append_durable()` of `OrderIntentCreated` returns `Acked`** (§6.2) — fail closed (`AuditWriteNotAcked`) otherwise, before CONFIRM/port-validity checks even run.
6. Operator CONFIRM + submit-port-validity + in-flight registration (unchanged from the existing orchestrator).
7. **`DurableAuditSink::append_durable()` of `OrderSubmitted` returns `Acked`** (§6.2) — fail closed (`AuditWriteNotAcked`) otherwise; this is strictly before the network call.
8. Sign via `BinanceSigner::sign()` over the exact bytes to be sent.
9. Send via Beast, honoring per-phase deadlines.
10. `transport_policy::check_http_status()` — reject 3xx (no auto-follow).
11. `transport_policy::check_response_size()` — cap response body size.
12. Parse response JSON via simdjson; run §4.2's full schema + field-match validation **before** any status-based branching.
13. Parse rate-limit headers (L4 §7) regardless of outcome, feed into `RequestWeightTracker::correct_from_response_header()` / order-count tracker / `freeze_until()` as appropriate.

## 4. Response mapping (exhaustive — replaces revision 1's 4-row table)

### 4.1 ABI additions this ambiguity/fill-state model actually requires

Revision 2 named `RateLimited` and the `Accepted`/`PartialFill`/`Filled` distinction but the current ABI has nowhere to put them: `SubmitOutcome` has 4 values, `SubmitResponse` carries no fill data, `OrchestratorGate` has no corresponding entries, and the orchestrator's result-routing switch only ever produces `Accepted`. These are the concrete additions (proposed, not yet implemented — this is what implementation would change):

```cpp
// live_submit_orchestrator.hpp — SubmitOutcome gains 2 values:
enum class SubmitOutcome : std::uint8_t {
    Accepted = 0,          // unchanged: matching-engine acknowledged (NEW)
    Rejected = 1,          // unchanged
    Timeout = 2,           // unchanged
    NetworkError = 3,      // unchanged
    RateLimited = 4,       // NEW — 429/418 received AFTER the request was sent.
                            // Fixes round-3 P0: this is NOT "never reached order
                            // processing" — by the time any HTTP response exists,
                            // Submitting/in-flight registration has already
                            // happened (Gate 12b/12c precede the actual send).
                            // Routes through Ambiguous like Timeout/NetworkError
                            // (§4.3) — Binance does not document 429/418 as a
                            // pre-matching-engine guarantee, so this cannot be
                            // treated as safely "never submitted."
    StaleRulesVersion = 5, // NEW — L4 §5.3's local pre-flight fail-closed check.
                            // Unlike RateLimited, this genuinely never sends
                            // anything: it's caught before request formatting,
                            // before Submitting/in-flight registration — the
                            // one case in this enum where "never touched the
                            // network" is actually true.
};

// SubmitResponse gains fill-detail fields, populated only when outcome
// indicates the matching engine actually processed the order:
struct SubmitResponse {
    SubmitOutcome outcome{SubmitOutcome::NetworkError};
    std::int64_t exchange_order_id{0};
    std::int32_t error_code{0};
    // NEW:
    OrderState exchange_status{OrderState::Ambiguous}; // NEW/PARTIALLY_FILLED/FILLED
                                                         // from the response `status` field —
                                                         // this, not fills[] presence, drives
                                                         // downstream lifecycle routing (§4.2)
    std::int64_t filled_qty_ticks{0};      // populated for PARTIALLY_FILLED/FILLED
    std::int64_t avg_fill_price_ticks{0};  // populated for PARTIALLY_FILLED/FILLED —
                                            // both map directly onto OrderRecord's
                                            // EXISTING fields of the same name
                                            // (order_lifecycle.hpp), no new storage needed
    std::uint32_t retry_after_seconds{0};  // populated when outcome == RateLimited
};

// live_submit_orchestrator.hpp — OrchestratorGate gains 3 values:
enum class OrchestratorGate : std::uint8_t {
    // ... existing 18 values unchanged ...
    SubmitPartialFill = 18,     // NEW — success terminal, not a failure gate
    SubmitFilled = 19,          // NEW — success terminal, not a failure gate
    SubmitRateLimited = 20,     // NEW — routes to Ambiguous (§4.3); freezes
                                 // RequestWeightTracker + OrderCountTrackerSet
                                 // (L4 §7); clientOrderId STAYS in InFlightRegistry
                                 // (fixes round-3 P0 — see SubmitOutcome::RateLimited's
                                 // comment above for why release-on-RateLimited was wrong)
    SubmitStaleRulesVersion = 21, // NEW — local fail-closed, genuinely never sent;
                                   // clientOrderId released (never submitted) —
                                   // this one IS safe to release, unlike RateLimited
    AuditWriteNotAcked = 22,    // NEW — see §6.2
};
```

### 4.2 `FULL` response schema validation (fixes rev-2 P1: `fills[]` presence is not the branching signal)

`newOrderRespType=FULL` guarantees Binance's own New Order response schema, including `fills[]` for filled quantity — but revision 2 implied `fills[]` *presence* was itself the signal that distinguished a fill from a plain acceptance. That's backwards: **`status` drives the branch; every other field is validated for consistency with the original request, not used to infer the branch.** Before any outcome is decided:

1. Parse the full response schema — required fields: `symbol`, `orderId`, `clientOrderId`, `transactTime`, `price`, `origQty`, `executedQty`, `status`, `side`. Missing any of these → treat as schema-invalid (§4.4's 200-with-schema-mismatch row).
2. Cross-check against the original request, all of which must match exactly: `symbol`, `clientOrderId`, `side`, `price` (compared as the same scaled-integer representation used to construct the request, not a floating-point string comparison).
3. Only after 1–2 pass does `status` (`NEW` / `PARTIALLY_FILLED` / `FILLED` / others) select the branch in §4.3's routing table. `fills[]` is read *after* the branch is already decided, purely to populate `filled_qty_ticks`/`avg_fill_price_ticks` — its presence or absence never decides which branch was taken.

### 4.3 Result routing (was: implicit "orchestrator transitions to Accepted"; now: explicit per-outcome routing)

| `SubmitOutcome` | `OrchestratorGate` | `OrderRecord` transition | `InFlightRegistry` |
|---|---|---|---|
| `Accepted`, `exchange_status == NEW` | `SubmitAccepted` | `Ambiguous`-free path: `Submitting → Accepted` (unchanged from current code) | stays registered (order live on exchange) |
| `Accepted`, `exchange_status == PARTIALLY_FILLED` | `SubmitPartialFill` (**new**) | `Submitting → Accepted → PartialFill`, `filled_qty_ticks`/`avg_fill_price_ticks` populated from `SubmitResponse` | stays registered |
| `Accepted`, `exchange_status == FILLED` | `SubmitFilled` (**new**) | `Submitting → Accepted → Filled` (or the direct `Submitting → Filled` transition `order_lifecycle.hpp` already permits for immediate fills) | released (terminal) |
| `Rejected` | `SubmitRejected` | `Submitting → Rejected` (unchanged) | released (terminal) |
| `Timeout` / `NetworkError` | `SubmitAmbiguous` / `SubmitNetworkError` | `Submitting → Ambiguous` (unchanged) | stays registered (unresolved) |
| `RateLimited` (**new**, fixed round-3) | `SubmitRateLimited` (**new**) | `Submitting → Ambiguous` — **identical treatment to Timeout/NetworkError.** By the time this response exists, `Submitting`/in-flight registration already happened; nothing distinguishes a 429/418 from any other post-send uncertainty except that its cause is known (rate limiting) | **stays registered** — reconciled via L4 §6 exactly like any other `Ambiguous` order; additionally freezes `RequestWeightTracker`/`OrderCountTrackerSet` per `Retry-After` so the *next* attempt (whether this order's reconciliation query or a future order's submit) waits appropriately |
| `StaleRulesVersion` (**new**, local-only, genuinely pre-send) | `SubmitStaleRulesVersion` (**new**) | No transition — caught at Gate 4 (§3), before `Submitting`, before in-flight registration | released — this is the one outcome in this table where "never touched the network" is actually true |

Reconciliation `Found` results (L4 §6.5) route into this same table via their mapped confirmed state — `Accepted`/`PartialFill`/`Filled`/`Rejected`/`Cancelled`/`Expired` — never through a separate "Reconciled" terminal that would hide a still-open order.

**Principle**: default to `Ambiguous` unless a response is unambiguously, schema-validated interpretable as accepted or cleanly rejected *before* the matching engine — and "before the matching engine" is provable only for responses that occur before any network call is made (`StaleRulesVersion`) or for HTTP-level conditions L4/this spec have positively confirmed occur pre-matching-engine (§4.4's allowlist). Everything else, including `RateLimited`, defaults to `Ambiguous`.

### 4.4 HTTP-level condition → outcome mapping

| Condition | `SubmitOutcome` | Notes |
|---|---|---|
| HTTP 200, passes §4.2's full schema + field-match validation | `Accepted` | Branch then selected by `status` per §4.2/§4.3 — never by `fills[]` presence. |
| HTTP 200, but body fails to parse, schema mismatch, or any of §4.2's cross-checked fields (`symbol`/`clientOrderId`/`side`/`price`) doesn't match the sent request | `NetworkError` → `Ambiguous` | A 200 status code alone is never sufficient — the payload must positively identify *this exact* order. |
| HTTP 400 with a code from §4.4.1's allowlist below | `Rejected` | Order never reached matching-engine acceptance — but only for the specific, enumerated codes; anything else defaults to the next row. |
| HTTP 400 with any code NOT on §4.4.1's allowlist (fixes round-3 P1 — "e.g." was not exhaustive) | `NetworkError` → `Ambiguous` | Fail closed on unfamiliar codes rather than guessing they're safe to treat as a clean rejection. |
| HTTP 400 with `-1021` | *(handled at L4 §2.2, not here)* | Forces a clock resync; not a terminal outcome for this order — the request is retried only after resync, still respecting no-blind-retry (a pre-send correction, not a post-send retry of an ambiguous state). |
| HTTP 403 | `Rejected` | Blocked before reaching the matching engine (WAF/geo); nothing was submitted. |
| HTTP 429 (fixed round-3 — was incorrectly `RateLimited`-as-never-submitted) | `RateLimited` → routes to `Ambiguous` (§4.3) | Read `Retry-After`, call `RequestWeightTracker::freeze_until()` + `OrderCountTrackerSet::freeze_all_until()` (L4 §7.2) — freezes **all** sends. Does **not** release in-flight; reconciled via L4 §6 like any other `Ambiguous` order. |
| HTTP 418 (IP auto-ban) | `RateLimited` → `Ambiguous` | Same handling as 429, typically longer `Retry-After`. |
| HTTP 5xx (500/502/503/504), including error code `-1007` (matching-engine timeout) | `NetworkError` → `Ambiguous` | The request may or may not have reached the matching engine — must reconcile via L4 §6. |
| Connect timeout / read timeout | `Timeout` → `Ambiguous` | |
| DNS failure / connection reset / TLS handshake failure | `NetworkError` → `Ambiguous` | |
| Truncated/incomplete response body (connection dropped mid-read) | `NetworkError` → `Ambiguous` | Never assume success on partial data. |
| Local `rules_version` mismatch (before any request is sent) | `StaleRulesVersion` | Never reaches the network — see §2.1/L4 §5.3. This is the one condition in this table that genuinely precedes `Submitting`/in-flight registration. |

### 4.4.1 HTTP 400 error-code allowlist (fixes round-3 P1: "e.g." was not exhaustive)

Only codes on this list may be classified `Rejected`. This list must be validated and, if necessary, extended by whoever implements this against Binance's current published error-code documentation at implementation time — Binance's list can change, and treating an unrecognized code as `Rejected` by default (rather than `Ambiguous`) is exactly the kind of silent-optimism this whole spec exists to prevent.

| Code | Meaning | Why it's safe to treat as `Rejected` |
|---|---|---|
| `-1013` | Filter failure (`LOT_SIZE`/`PRICE_FILTER`/`MIN_NOTIONAL`, etc.) | Input-validation layer, before matching-engine routing. |
| `-1100` | Illegal characters in a parameter | Input-validation layer. |
| `-1101`/`-1104`/`-1105`/`-1106` | Parameter count/format errors | Input-validation layer. |
| `-1102`/`-1103` | Missing/unknown mandatory parameter | Input-validation layer. |
| `-1111` | Precision beyond what the symbol allows | Input-validation layer. |
| `-1116` | Invalid order type | Input-validation layer (this codebase only ever sends `LIMIT`, so this indicates a local bug, not exchange state — still safely `Rejected`, never `Ambiguous`). |
| `-1117` | Invalid side | Input-validation layer, same reasoning as `-1116`. |
| `-1118` | Empty `newClientOrderId` | Input-validation layer — indicates a local bug in ID generation, not exchange state. |
| `-1121` | Invalid symbol | Input-validation layer. |
| `-2010` | New order rejected (e.g. insufficient balance) | Documented as an order-validation rejection, not a matching-engine-state-dependent outcome. |

Any code not on this list — including any future Binance code this list hasn't been updated for — defaults to `Ambiguous`.

## 5. Reconciliation wiring

Any `Ambiguous` outcome (§4.3 — including `Timeout`, `NetworkError`, and, after round-3's fix, `RateLimited`) routes to L4 §6.2's orchestrator-level reconciliation loop, which calls `order_lifecycle.hpp::determine_reconcile_action()` to decide `QueryOrder` vs. `EscalateToOperator`, calling `BinancePrivateRestClient::query_order()` (§2) — a single attempt per call — for the former, incrementing and durably persisting `OrderRecord::query_attempts` after each attempt.

L4's revision 3 does propose one change to the state machine: `order_lifecycle.hpp::validate_transition()`'s `Ambiguous` case is extended (L4 §6.5) to permit transitioning into the real confirmed state (`Accepted`/`PartialFill`/`Filled`/`Rejected`/`Cancelled`/`Expired`) rather than forcing every `Found` result through the terminal `Reconciled` state, which would incorrectly mark a still-open order as done. This is the one place across both specs where an existing state machine's transition table needs an actual code change, not just a new caller wired into it.

## 6. Persistent audit — hard precondition, not deferred

`AuditRingSink` (`audit_trail.hpp`) is explicitly an in-memory, test/first-path sink (F5 fix made it fail-closed *once full*, but it still doesn't survive a process crash). Revision 2 named this as a precondition but defined no interface, no ACK contract, no recovery path — and a direct read of `live_submit_orchestrator.hpp` confirms **every one of its 16 `ctx.audit->append(ar)` call sites ignores the return value**. The "audit confirmed before POST" claim was not actually true of the current code; this revision fixes that gap concretely, not just in prose.

### 6.1 `DurableAuditSink` interface (proposed — replaces/extends `AuditRingSink` for L5 use)

```cpp
enum class AuditAppendResult : std::uint8_t {
    Acked = 0,   // durably written and confirmed (fsync'd file append return, or
                 // committed DB transaction) — the write is guaranteed to survive
                 // a process crash occurring immediately after this call returns.
    Failed = 1,  // write attempted but durability could not be confirmed —
                 // treat identically to "audit unavailable" (ADR-019 D10).
};

class DurableAuditSink {
public:
    virtual ~DurableAuditSink() = default;

    // Blocking until durably persisted or failed. Never returns Acked before
    // the underlying storage confirms the write.
    virtual AuditAppendResult append_durable(const AuditRecord& rec) noexcept = 0;

    // Called once at process startup, before any order processing begins.
    // Replays the durable log to reconstruct any in-flight orders that were
    // mid-submission when the process last exited or crashed, repopulating
    // InFlightRegistry so a resumed process cannot accidentally treat a
    // pre-crash in-flight order as available for resubmission.
    virtual void recovery_scan(InFlightRegistry& out_registry) noexcept = 0;
};
```

`AuditRingSink` remains as-is for tests and non-L5 paths (dry-run, unit tests); `DurableAuditSink` is a new, separate interface `binance_private_rest.hpp`'s runtime wiring requires — this design does not ask `AuditRingSink` itself to become durable, since that would break its existing test usage.

### 6.2 Orchestrator wiring (was: "audit confirmed" as prose; now: the actual gate)

Fixes round-3 P1: `ctx.durable_audit` was used below without being declared anywhere, and no null behavior was defined.

```cpp
// live_submit_orchestrator.hpp — OrchestratorContext gains a required field:
struct OrchestratorContext {
    // ... existing fields ...
    DurableAuditSink* durable_audit{nullptr};  // NEW — required for the L5 path;
                                                 // null is checked explicitly below,
                                                 // matching the existing pattern for
                                                 // ctx.audit (AuditRingSink) at Gate 1.
};

// Gate 6 (currently a fire-and-forget append(ar) with the return value
// discarded) becomes:

if (!ctx.durable_audit) {
    result.gate = OrchestratorGate::AuditWriteNotAcked;  // null sink fails closed
                                                           // identically to a failed write —
                                                           // there is no "proceed without
                                                           // durable audit" path
    return result;
}

AuditRecord intent_record = /* ... OrderIntentCreated, as today ... */;
if (ctx.durable_audit->append_durable(intent_record) != AuditAppendResult::Acked) {
    result.gate = OrchestratorGate::AuditWriteNotAcked;  // new gate value, §4.1
    return result;  // fail-closed — no further processing, no POST
}

// ... CONFIRM check, port-validity check, in-flight registration (unchanged) ...

AuditRecord submitted_record = /* ... OrderSubmitted ... */;
if (ctx.durable_audit->append_durable(submitted_record) != AuditAppendResult::Acked) {
    result.gate = OrchestratorGate::AuditWriteNotAcked;
    return result;  // fail-closed — the POST has NOT been sent yet at this point;
                     // this check happens strictly before the network call
}

// Only here does the actual POST (BinancePrivateRestClient::submit_order()) fire.
```

This is the concrete difference from revision 2: the durability check is a real gate with a real failure path (`AuditWriteNotAcked`), evaluated with the actual `append_durable()` return value inspected — not an assumption that calling `append()` (fire-and-forget, as all 16 existing call sites do today) constitutes "confirmed."

### 6.3 Why this can't wait

A crash between "POST sent" and "outcome recorded" is exactly the ambiguous-order scenario this whole gate chain exists to handle safely — and it's unrecoverable if the *intent itself* wasn't durably recorded first. Deferring persistent audit "for later" means the highest-risk moment (an in-flight order during a crash) is the one moment this design can't account for. §6.1's `recovery_scan()` is the other half of this: a crash-and-restart must reconstruct in-flight state from the durable log, not start `InFlightRegistry` empty and risk a duplicate submission of an order that was actually still pending when the process died.

## 7. What this design deliberately does not do

- Does not implement `binance_private_rest.hpp` — that's the actual implementation, gated on this spec's acceptance plus L4's.
- Does not implement a concrete `DurableAuditSink` backend (file-append vs. DB) — §6.1 specifies the interface contract; choosing and implementing a backend is a separate, smaller decision that can happen once this interface is accepted.
- Does not implement or propose an implementation timeline.
- Does not add auto-detection of environment (testnet vs production) — that remains an explicit, frozen-at-startup operator choice (L4 §1), never inferred.

## Before implementation begins

1. `docs/BINANCE_PRIVATE_REST_L4_SPEC.md` accepted first — this spec is not self-sufficient without it.
2. This spec itself needs Architect acceptance.
3. A real dry-run harness exercising this shape against Binance **testnet** (owner-provided testnet credentials, never production).
4. A concrete `DurableAuditSink` backend exists and is wired in per §6 — not deferred, and not satisfied by `AuditRingSink`.
5. Independent review of the actual implementation once written.
6. The owner's own explicit decision to authorize anything beyond testnet — no role in this repo's process grants that on its own.
