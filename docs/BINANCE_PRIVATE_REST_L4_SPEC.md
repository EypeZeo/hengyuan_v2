# Binance Private REST — L4 Foundation Spec (Draft, Not Implemented)

## Status

**Draft, revision 2.** No code from this spec has been written. Per `docs/NATIVE_ARCHITECTURE.md`'s gate table, everything in this spec is **L4** — signed *read-only* requests. Nothing here submits an order; `docs/SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` (L5, POST) depends on this spec's primitives and must not be implemented before this one is accepted.

This spec exists because the original L5-only draft was rejected on review (see that file's changelog) for treating reconciliation, environment binding, signing discipline, and rate-limit accounting as someone else's problem "for later." They aren't — they're the shared foundation both reconciliation-after-ambiguous and the POST adapter need, and per ADR-019 D8, reconciliation specifically cannot be deferred past the first live submit path.

Revision 2 (this revision) fixes 3 P0 and 2 P1 findings from the second Architect review round: exhaustive reconciliation-query failure semantics (§6), concrete `rules_version` data model and call-chain wiring (§5), factory-only `EnvironmentBinding` construction (§1), and a concrete `RequestWeightTracker` correction/freeze API + order-count bucket (§7). See `SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md`'s revision 3 changelog for the full point-by-point mapping — both files were revised together in response to the same review.

## 1. Environment binding (fixes: testnet unreachable in the original draft; fixes rev-2 P1: was a freely-constructible aggregate, not actually structurally exclusive)

The original draft defaulted to `api.binance.com` with no testnet in the allowlist, while separately requiring a testnet run first — self-contradictory. Revision 2 fixed the allowlist split but left `EnvironmentBinding` as a public aggregate — anyone could construct `EnvironmentBinding{Testnet, "api.binance.com", prod_policy, prod_cred_key}` and the type system would accept it, which contradicts the "structurally impossible to cross environments" claim. Fix: constructor is private; only two named factories can produce a valid instance, each hardcoding its own correct host+allowlist pairing so the caller cannot mix them.

```cpp
enum class BinanceEnvironment : std::uint8_t {
    Testnet = 0,
    Production = 1,
};

class EnvironmentBinding {
public:
    // Only these two factories construct a valid instance. Each hardcodes the
    // correct (environment, base_host, allowlist) triple internally — the
    // caller supplies only the credential env-var key name, so there is no
    // parameter combination that produces a Testnet-enum-with-production-host
    // (or vice versa) instance. This is what makes the exclusivity structural
    // rather than a documentation promise.
    static EnvironmentBinding testnet(std::string_view secret_env_key) noexcept {
        return EnvironmentBinding(BinanceEnvironment::Testnet, "testnet.binance.vision",
                                   testnet_transport_policy(), secret_env_key);
    }
    static EnvironmentBinding production(std::string_view secret_env_key) noexcept {
        return EnvironmentBinding(BinanceEnvironment::Production, "api.binance.com",
                                   production_transport_policy(), secret_env_key);
    }

    BinanceEnvironment environment() const noexcept { return environment_; }
    std::string_view base_host() const noexcept { return base_host_; }
    const TransportPolicy& transport_policy() const noexcept { return transport_policy_; }
    std::string_view credential_env_key() const noexcept { return credential_env_key_; }

private:
    EnvironmentBinding(BinanceEnvironment env, std::string_view host,
                       TransportPolicy policy, std::string_view cred_key) noexcept
        : environment_(env), base_host_(host), transport_policy_(policy),
          credential_env_key_(cred_key) {}

    // Returns a TransportPolicy whose endpoint_allowlist contains ONLY
    // testnet.binance.vision — never shares state with production_transport_policy().
    static TransportPolicy testnet_transport_policy() noexcept;
    // Returns a TransportPolicy whose endpoint_allowlist contains ONLY
    // api.binance.com + api1/2/3.binance.com — never shares state with testnet.
    static TransportPolicy production_transport_policy() noexcept;

    BinanceEnvironment environment_;
    std::string_view base_host_;
    TransportPolicy transport_policy_;
    std::string_view credential_env_key_;
};
```

Rules:

- An `EnvironmentBinding` is chosen once at process startup via `EnvironmentBinding::testnet(...)` or `::production(...)` and is **immutable** for the process lifetime — no setter, no runtime flag to flip it later, no public constructor to bypass the factories.
- The two `*_transport_policy()` functions are the only place either allowlist is defined; they must never read from a shared/parameterized source that could let one environment's allowlist leak into the other.
- The credential env-var key is environment-specific (e.g. caller passes `"HENGYUAN_BINANCE_TESTNET_SECRET"` to `testnet()`, `"HENGYUAN_BINANCE_LIVE_SECRET"` to `production()`) so `env_loader.hpp`'s allowlist (`EnvAllowlist`) naturally rejects a key presented under the wrong variable name.

## 2. Signing discipline (fixes: query-string/percent-encoding/timestamp gaps)

### 2.1 Query string construction

- Parameters are assembled in a **fixed, deterministic field order** (this spec picks alphabetical-by-key; Binance doesn't require a specific order, but signing must be deterministic and testable).
- **No duplicate parameter names** ever.
- Any parameter value containing characters outside unreserved RFC 3986 (`A-Za-z0-9-._~`) is percent-encoded **before** the signature is computed.
- **Invariant: signed bytes == sent bytes.** The exact byte sequence HMAC-signed by `binance_signer.hpp` must be byte-identical to what's transmitted on the wire. No re-encoding after signing, no signing a "logical" string that differs from the transmitted one — this is the single most common source of real-world Binance signature-mismatch (`-1022`) bugs.

### 2.2 Timestamp and clock sync

- `timestamp` is generated as the **last step** before signing — never cached from earlier in request construction, to minimize clock-skew exposure.
- `recvWindow` is fixed at `transport_policy.hpp::TransportPolicy::recv_window_ms` (currently defaults to 5000ms) and must never exceed Binance's documented hard cap (**60000ms**) — this spec freezes it at 5000ms, full stop, no per-call override.
- **Server-time offset**: maintained as `offset_ms = server_time - (local_send_time + rtt/2)` (classic NTP-style symmetric-latency approximation), computed from `GET /api/v3/time` (§3, public, unauthenticated).
  - **TTL**: offset is considered fresh for 5 minutes after fetch.
  - **Refresh**: attempted proactively before the TTL expires, on a background/init path, not inline with an order-critical request.
  - **Refresh failure**: if refresh fails but the cached offset is still within TTL, keep using it. If TTL has expired and refresh fails, **fail closed** — do not send any signed request with a potentially-stale offset.
  - **`-1021` (`Timestamp for this request is outside of the recvWindow`)**: on receipt, immediately force a resync (re-call `GET /api/v3/time`) before any further signed request. Do **not** blindly retry the same request with the same (mis-synced) clock — this is the same no-blind-retry principle `order_lifecycle.hpp` already enforces for ambiguous submits, applied to clock skew.

## 3. `GET /api/v3/time` (public, unauthenticated)

Returns `{"serverTime": <ms>}`. Used only for §2.2's offset calculation. No signing required.

## 4. `GET /api/v3/account` (signed, USER_DATA)

Backs `account_truth.hpp`'s `AccountSnapshot`. Same signing pipeline as §2. Response populates the existing `AccountSnapshot` struct; the existing `check_freshness()` gate in `account_truth.hpp` is unchanged — this section only specifies *how the network fetch that feeds it* is signed and validated, not the freshness policy itself (that's already spec'd correctly in ADR-019 D6).

## 5. `GET /api/v3/exchangeInfo` (public, unauthenticated, large response) — Symbol Registry

Fixes: SubmitPort's ABI only carries `symbol_id` + integer ticks, but Binance's wire format needs a `symbol` string and decimal `price`/`quantity` strings matching exchange filter precision. Revision 2 named this gap (`SymbolRegistryEntry`, `rules_version`) but only as prose — `account_truth.hpp`'s actual `SymbolRules` struct has no `rules_version`, `price_scale`, or `qty_scale` field, and `validate_pre_trade()`'s signature has no way to report which version it validated against. This revision fixes the data model and the call-chain wiring, not just the intent.

### 5.1 Extend `SymbolRules` — one struct, not two competing ones

Rather than introducing a separate `SymbolRegistryEntry` that could drift out of sync with `account_truth.hpp`'s existing `SymbolRules`, **extend `SymbolRules` itself** with the three missing fields:

```cpp
// Proposed addition to native/include/hengyuan/account_truth.hpp's SymbolRules:
struct SymbolRules {
    char symbol[kSymbolNameLen]{};
    bool is_trading{false};
    std::int64_t min_qty_ticks{0};
    std::int64_t max_qty_ticks{0};
    std::int64_t step_size_ticks{0};
    std::int64_t min_price_ticks{0};
    std::int64_t max_price_ticks{0};
    std::int64_t tick_size_ticks{0};
    std::int64_t min_notional_ticks{0};

    // NEW — required for wire-format construction (§5.2) and version binding (§5.3):
    std::uint8_t price_scale{};      // decimal places implied by tick_size_ticks
    std::uint8_t qty_scale{};        // decimal places implied by step_size_ticks
    std::uint32_t rules_version{};   // shared across the whole registry snapshot this
                                     // entry came from — see §5.3, not per-symbol-independent
};
```

One registry, one refresh operation, one `rules_version` for the whole snapshot (all symbols refreshed atomically together) — not a per-symbol version that could have some symbols stale and others fresh within the same submitted order.

### 5.2 Lossless tick→decimal formatting

Converting an integer tick count back to Binance's expected decimal string uses integer arithmetic (division/modulo against `10^price_scale` / `10^qty_scale`), never a floating-point round-trip — matching this codebase's existing `fixed_point.hpp` discipline. No scientific notation, no silently-dropped trailing precision.

### 5.3 Carrying `rules_version` through the call chain (was: "must be cross-checked", now: concrete signatures)

```cpp
// account_truth.hpp — validate_pre_trade() gains an out-parameter reporting
// which rules_version it validated against, so the caller can carry it forward:
PreTradeCheck validate_pre_trade(
    const SymbolRules& rules, OrderSide side, std::int64_t price_ticks,
    std::int64_t qty_ticks, const AccountSnapshot& account,
    std::string_view base_asset, std::string_view quote_asset,
    std::int64_t now_ms, const ExposureLimits& limits,
    std::uint32_t& validated_rules_version_out) noexcept;  // NEW out-param

// live_submit_orchestrator.hpp — OrchestratorContext carries the version
// forward from Gate 6+7 (pre-trade) to Gate 12 (submit):
struct OrchestratorContext {
    // ... existing fields ...
    std::uint32_t pre_trade_rules_version{0}; // set by the orchestrator after
                                               // validate_pre_trade() succeeds
};

// binance_private_rest.hpp (SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md's §2) —
// submit_order() receives the SAME version pre-trade validated against, and
// must compare it to its own currently-loaded registry version before
// formatting the wire request:
SubmitResponse submit_order(
    std::string_view client_order_id, std::uint32_t symbol_id,
    OrderSide side, OrderType type,
    std::int64_t price_ticks, std::int64_t qty_ticks,
    std::uint32_t expected_rules_version) noexcept;  // NEW param
// If expected_rules_version != registry_.current_version(), the request is
// NEVER formatted or sent — return SubmitOutcome::StaleRulesVersion (a new,
// visually distinct outcome, never conflated with an exchange-side Rejected
// or a NetworkError — this is a purely local, pre-flight fail-closed check).
```

This is the concrete mechanism: the version travels as a real function parameter from `validate_pre_trade()` through `OrchestratorContext` to `submit_order()`, and a mismatch is a distinct, checkable outcome — not a prose promise that two independently-refreshed structures "should" agree.

## 6. `GET /api/v3/order` (signed, reconciliation query) — fixes: reconciliation was explicitly out of scope, must not be

ADR-019 D8 requires reconciliation-after-ambiguous to exist before any live POST path is authorized. The original draft excluded it "for later" — the review's first blocking finding. Revision 2 fixed the omission but only defined retry semantics for one specific error code (`-2013`); it left every other failure mode of the query *itself* (5xx, `-1007`, `429`/`418`, DNS/TLS failure, timeout, truncated body, 200-with-schema-mismatch, 200-with-COID-mismatch) undefined. Revision 3 fixes this by collapsing every non-definitive response into one explicit outcome, so there is no code path left that could accidentally treat "the query failed" as "the order doesn't exist."

```
GET /api/v3/order?symbol={symbol}&origClientOrderId={coid}&recvWindow={ms}&timestamp={now}&signature={...}
```

Same signing pipeline as everything else in this spec (§2).

### 6.1 Query outcome model

```cpp
enum class ReconcileQueryOutcome : std::uint8_t {
    Found = 0,         // Binance definitively confirmed the order's state.
    Inconclusive = 1,  // The query itself did not produce a trustworthy answer —
                       // see the exhaustive list below. This is the ONLY other
                       // outcome; there is no "confirmed does not exist" outcome.
};

struct ReconcileQueryResult {
    ReconcileQueryOutcome outcome;
    OrderState confirmed_state{OrderState::Ambiguous}; // valid only if outcome == Found
    std::int64_t exchange_order_id{0};                 // valid only if outcome == Found
    std::uint32_t retry_after_seconds{0};               // valid only if Inconclusive was
                                                         // caused by 429/418 — see §6.3
};
```

**Every one of the following collapses to `Inconclusive`** — none of them, individually or repeated, may ever be read as "the order does not exist" or as any other definitive conclusion:

- `-2013 Order does not exist` (the specific case revision 2 handled) — Binance's Memory→Database visibility lag means this can be transiently wrong even when the order was accepted moments earlier.
- HTTP 5xx (including `-1007` matching-engine timeout).
- HTTP `429`/`418` (rate limited — `retry_after_seconds` populated from the `Retry-After` header).
- DNS failure, connection reset, TLS handshake failure, connect/read timeout.
- Truncated/incomplete response body.
- HTTP 200 but the body fails schema validation, or the returned `origClientOrderId` doesn't match what was queried.

### 6.2 Retry policy

On `Inconclusive`, retry the *same query* (never the original POST — that remains forbidden by idempotency) up to `order_lifecycle.hpp::OrderRecord::kMaxQueryAttempts` (already defined as 3), with backoff `1s, 2s, 4s`. This is the network call that `order_lifecycle.hpp::determine_reconcile_action()`'s `QueryOrder` action already models in the state machine — this section defines what actually executes when that action fires, and it always returns one of the two outcomes above, never a third "definitively absent" state.

### 6.3 Rate-limit interaction

If a retry's `Inconclusive` was caused by `429`/`418`, the next attempt's wait time is `max(scheduled_backoff, retry_after_seconds)` — the query loop must never re-send while a rate-limit freeze (§7) is in effect, even if the fixed backoff schedule would otherwise allow it sooner.

### 6.4 Exhaustion

If all `kMaxQueryAttempts` (3) queries return `Inconclusive`, the existing state machine's `EscalateToOperator` path fires — **this is "we don't know, a human decides," never "the order was confirmed absent."** No new logic is needed in `order_lifecycle.hpp` for this; it already resolves correctly. What was missing was this network implementation feeding it real, exhaustively-categorized answers instead of a mock that only ever returned one shape of failure.

### 6.5 Transition on `Found`

`order_lifecycle.hpp::validate_transition()` only permits `Ambiguous → Reconciled` or `Ambiguous → EscalatedToOperator` — there is no direct `Ambiguous → Filled`/`PartialFill` transition. A `Found` result therefore transitions the record to `Reconciled` (the only legal target); `ReconcileQueryResult::confirmed_state` is recorded as auxiliary audit detail (what Binance actually reported) alongside that transition, not used to bypass the state machine's transition table.

## 7. Rate-limit header accounting (fixes: local-only weight=1 assumption; fixes rev-2 P1: "feed back" had no concrete API)

After **every** response (success or error), parse:

- `X-MBX-USED-WEIGHT-*` (interval-suffixed, e.g. `X-MBX-USED-WEIGHT-1M`) — Binance's authoritative view of request-weight consumption.
- `X-MBX-ORDER-COUNT-*` (e.g. `X-MBX-ORDER-COUNT-10S`, `X-MBX-ORDER-COUNT-1D`) — **a separate rate-limit bucket from request weight**, enforced independently by Binance, specific to order-placing endpoints.
- `Retry-After` (present on `429`/`418` responses, seconds).

Revision 2 said this should "feed back into `RequestWeightTracker`" without defining how. Concretely, `RequestWeightTracker` (`transport_policy.hpp`) gains two new methods, and orchestration wires up a **second, separately-configured instance** for order-count (Binance enforces weight and order-count as independent buckets; one tracker's single-window model cannot represent both):

```cpp
class RequestWeightTracker {
public:
    // ... existing reset/try_consume/can_send/used/remaining, unchanged ...

    // NEW: corrects the local estimate toward Binance's authoritative reported
    // value after a response. Takes max(local_estimate, server_reported) so a
    // disagreement never makes the tracker LESS conservative than before.
    void correct_from_response_header(std::uint32_t server_reported_used_weight,
                                       TimePoint now = Clock::now()) noexcept;

    // NEW: global freeze, independent of the weight/window accounting above.
    // Set on 429/418 from ANY call site (order submit, reconciliation query,
    // account fetch — anything using this environment's transport). While
    // frozen, can_send()/try_consume() both return false regardless of
    // remaining weight budget.
    void freeze_until(TimePoint until) noexcept;
    bool is_frozen(TimePoint now = Clock::now()) const noexcept;
};
```

Orchestration wiring: one `RequestWeightTracker` instance tracks request weight (as today); a second, distinctly-configured instance tracks order-count (separate limit values, e.g. 10s/1d windows per Binance's published limits for the account). **Both** must pass their `can_send()` pre-check (L5 spec's Gate 8) before a submit attempt proceeds, and **either** tracker's `freeze_until()` being active blocks all sends until it clears — a 429 on the order-count bucket freezes exactly as hard as one on the weight bucket.

## 8. Security contract for this client — distinct from `binance_rest_snapshot.hpp`

Fixes: the original draft said "reuse `binance_rest_snapshot.hpp`'s pattern," which conflated *protocol shape* with *security guarantees*. Correct framing: **reuse the Boost.Beast plumbing structure (resolve → connect → TLS handshake → write → read → parse), do not assume its security posture carries over.** `binance_rest_snapshot.hpp` talks to a public, unauthenticated endpoint; a client carrying signed requests and (indirectly, via headers) API keys needs its own, independently specified and tested contract:

- Explicit hostname verification (SNI + certificate CN/SAN match against the bound environment's `base_host`) — must be a provable, tested property of *this* component, not inherited by assumption.
- Per-phase deadlines, not one shared timeout: separate budgets for DNS resolve, TCP connect, TLS handshake, write, and read — a slow DNS resolver shouldn't be able to consume the entire `connect_timeout_ms` budget and leave zero time for the actual connect.
- Response-size cap enforcement (`transport_policy.hpp::check_response_size()`) — reused as-is, but its enforcement point in *this* client must be tested directly, not assumed from the public snapshot client's tests.

## Before implementation begins

1. This spec itself needs Architect acceptance (same process that produced the review this spec responds to).
2. `docs/SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` depends on every section here and must not be implemented first.
3. A real dry-run harness exercising §3/§4/§5/§6 against Binance **testnet** (owner-provided testnet credentials, never production) before any of this is trusted.
4. Independent review of the actual implementation once written — this repo's own governance for anything execution-adjacent (see `CLAUDE.md`'s boundary section) still applies regardless of the lighter engineering-ceremony rules for ordinary native work.
