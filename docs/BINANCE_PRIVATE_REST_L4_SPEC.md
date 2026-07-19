# Binance Private REST — L4 Foundation Spec (Draft, Not Implemented)

## Status

**Draft, revision 3.** No code from this spec has been written. Per `docs/NATIVE_ARCHITECTURE.md`'s gate table, everything in this spec is **L4** — signed *read-only* requests. Nothing here submits an order; `docs/SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` (L5, POST) depends on this spec's primitives and must not be implemented before this one is accepted.

This spec exists because the original L5-only draft was rejected on review (see that file's changelog) for treating reconciliation, environment binding, signing discipline, and rate-limit accounting as someone else's problem "for later." They aren't — they're the shared foundation both reconciliation-after-ambiguous and the POST adapter need, and per ADR-019 D8, reconciliation specifically cannot be deferred past the first live submit path.

Revision 2 fixed 3 P0 and 2 P1 findings from the second Architect review round: exhaustive reconciliation-query failure semantics (§6), concrete `rules_version` data model and call-chain wiring (§5), factory-only `EnvironmentBinding` construction (§1), and a concrete `RequestWeightTracker` correction/freeze API + order-count bucket (§7).

**Revision 3** (this revision) fixes 4 more findings from round 3, two of them genuine safety bugs rather than missing detail:

- §6.5 — round 2's "`Found` → transition to `Reconciled`" would mark a still-open order (Binance status `NEW` or `PARTIALLY_FILLED`) as **terminal**, since `Reconciled` is one of `order_lifecycle.hpp::is_terminal()`'s terminal states. That stops the system from tracking a position that is, in fact, still live on the exchange — a real correctness bug, not just a modeling gap. Fixed: `Found` now maps to the actual confirmed state (`Accepted`/`PartialFill`/`Filled`/`Rejected`/`Cancelled`/`Expired`), and in-flight release happens only when that confirmed state is actually terminal.
- §6.2 — the retry loop's location was ambiguous, and nothing defined who increments/persists `OrderRecord::query_attempts` (the field `determine_reconcile_action()` actually reads). Fixed: the loop lives in the orchestrator, not inside `query_order()`; every attempt increments and durably persists the count before the next decision is made, and `recovery_scan()` must reconstruct it after a crash.
- §7 — order-count limits have multiple simultaneous intervals (10s, 1d, etc.); "a second tracker" can't represent that. Fixed: a small fixed-capacity tracker set, one per interval Binance actually reports.
- §1 — `EnvironmentBinding`'s factories still accepted a caller-supplied credential env-var name, which could itself be the wrong environment's name. Fixed: factories take no parameters; both the API-key and secret env-var names are hardcoded per environment.

See `SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md`'s revision 4 changelog for the full point-by-point mapping — both files were revised together in response to the same review.

## 1. Environment binding (fixes: testnet unreachable in the original draft; fixes rev-2 P1: was a freely-constructible aggregate, not actually structurally exclusive)

The original draft defaulted to `api.binance.com` with no testnet in the allowlist, while separately requiring a testnet run first — self-contradictory. Revision 2 fixed the allowlist split but left `EnvironmentBinding` as a public aggregate — anyone could construct `EnvironmentBinding{Testnet, "api.binance.com", prod_policy, prod_cred_key}` and the type system would accept it, which contradicts the "structurally impossible to cross environments" claim. Fix: constructor is private; only two named factories can produce a valid instance, each hardcoding its own correct host+allowlist pairing so the caller cannot mix them.

```cpp
enum class BinanceEnvironment : std::uint8_t {
    Testnet = 0,
    Production = 1,
};

class EnvironmentBinding {
public:
    // Only these two factories construct a valid instance, and — fixing round
    // 3's finding — they take NO parameters. Round 2 accepted a caller-supplied
    // secret_env_key string, which meant a caller could still call testnet()
    // but hand it "HENGYUAN_BINANCE_LIVE_SECRET" (the production name),
    // undermining the exclusivity claim at the one point that mattered most.
    // Both the API-key and secret env-var names are now hardcoded per
    // environment, inside the factory — there is no parameter left for the
    // caller to get wrong, for the host/allowlist pairing OR the credential names.
    static EnvironmentBinding testnet() noexcept {
        return EnvironmentBinding(BinanceEnvironment::Testnet, "testnet.binance.vision",
                                   testnet_transport_policy(),
                                   "HENGYUAN_BINANCE_TESTNET_API_KEY",
                                   "HENGYUAN_BINANCE_TESTNET_SECRET");
    }
    static EnvironmentBinding production() noexcept {
        return EnvironmentBinding(BinanceEnvironment::Production, "api.binance.com",
                                   production_transport_policy(),
                                   "HENGYUAN_BINANCE_LIVE_API_KEY",
                                   "HENGYUAN_BINANCE_LIVE_SECRET");
    }

    BinanceEnvironment environment() const noexcept { return environment_; }
    std::string_view base_host() const noexcept { return base_host_; }
    const TransportPolicy& transport_policy() const noexcept { return transport_policy_; }
    std::string_view api_key_env_key() const noexcept { return api_key_env_key_; }
    std::string_view secret_env_key() const noexcept { return secret_env_key_; }

private:
    EnvironmentBinding(BinanceEnvironment env, std::string_view host,
                       TransportPolicy policy, std::string_view api_key_env,
                       std::string_view secret_env) noexcept
        : environment_(env), base_host_(host), transport_policy_(policy),
          api_key_env_key_(api_key_env), secret_env_key_(secret_env) {}

    // Returns a TransportPolicy whose endpoint_allowlist contains ONLY
    // testnet.binance.vision — never shares state with production_transport_policy().
    static TransportPolicy testnet_transport_policy() noexcept;
    // Returns a TransportPolicy whose endpoint_allowlist contains ONLY
    // api.binance.com + api1/2/3.binance.com — never shares state with testnet.
    static TransportPolicy production_transport_policy() noexcept;

    BinanceEnvironment environment_;
    std::string_view base_host_;
    TransportPolicy transport_policy_;
    std::string_view api_key_env_key_;
    std::string_view secret_env_key_;
};
```

Rules:

- An `EnvironmentBinding` is chosen once at process startup via `EnvironmentBinding::testnet()` or `::production()` (no arguments) and is **immutable** for the process lifetime — no setter, no runtime flag to flip it later, no public constructor to bypass the factories.
- The two `*_transport_policy()` functions are the only place either allowlist is defined; they must never read from a shared/parameterized source that could let one environment's allowlist leak into the other.
- Both credential env-var names are hardcoded per environment inside the factory (`HENGYUAN_BINANCE_TESTNET_API_KEY`/`_SECRET` vs `HENGYUAN_BINANCE_LIVE_API_KEY`/`_SECRET`) — `env_loader.hpp`'s allowlist (`EnvAllowlist`) naturally rejects a key presented under the wrong variable name, and there is no code path by which a caller could request the wrong pairing.

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

### 6.2 Retry loop lives in the orchestrator, not inside `query_order()` (fixes round-3 P0: nothing defined who increments/persists `query_attempts`)

`query_order()` (the client method, `SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` §2) performs **exactly one** query attempt and returns — it does not loop internally. The retry loop, the `query_attempts` increment, and the durable persistence of each attempt all live in the orchestrator's reconciliation-handling code, because `order_lifecycle.hpp::determine_reconcile_action()` reads `OrderRecord::query_attempts` to decide `QueryOrder` vs. `EscalateToOperator` — if the count only lived inside a client-internal loop, that field would stay at 0 forever and the state machine could never naturally escalate.

```cpp
// Orchestrator-level reconciliation loop (proposed, not yet implemented):
void reconcile_ambiguous_order(OrderRecord& rec, BinancePrivateRestClient& client,
                                DurableAuditSink& audit) noexcept {
    while (true) {
        auto action = determine_reconcile_action(rec);  // reads rec.query_attempts
        if (action == ReconcileAction::EscalateToOperator) {
            rec.transition_to(OrderState::EscalatedToOperator);
            audit.append_durable(/* OrderEscalated, durably confirmed */);
            return;
        }
        // action == ReconcileAction::QueryOrder
        auto result = client.query_order(/* symbol, rec.client_order_id */);

        // Increment BEFORE the durable write, so a crash between "attempt made"
        // and "attempt persisted" undercounts (safe: retries one extra time)
        // rather than overcounts (unsafe: could escalate prematurely).
        rec.query_attempts++;
        audit.append_durable(/* AuditEventType::OrderReconciliationAttempted —
                                 NEW event type, includes rec.query_attempts and
                                 result.outcome, write confirmed before proceeding */);

        if (result.outcome == ReconcileQueryOutcome::Found) {
            // §6.5 below — maps to the real confirmed state, not a blanket terminal.
            apply_confirmed_state(rec, result, audit);
            return;
        }
        // Inconclusive: loop again. determine_reconcile_action() will now see
        // the incremented query_attempts and may return EscalateToOperator.
        sleep_respecting_backoff_and_freeze(rec.query_attempts, result.retry_after_seconds);
    }
}
```

`AuditEventType` (`audit_trail.hpp`) needs one new value — `OrderReconciliationAttempted` — since none of its 20 existing values represent a single reconciliation attempt (as distinct from the eventual `OrderReconciled`/`OrderEscalated` outcome).

**Recovery**: `DurableAuditSink::recovery_scan()` must reconstruct `query_attempts` from the durable log of `OrderReconciliationAttempted` events for each in-flight order, not just re-populate `InFlightRegistry` membership — a process restart mid-reconciliation must resume from the correct attempt count, not reset to 0 (which could either retry beyond the intended cap across restarts, or, if miscounted the other way, escalate too early).

### 6.3 Rate-limit interaction

If an attempt's `Inconclusive` was caused by `429`/`418`, the *next* attempt's wait time (computed by the orchestrator's loop, §6.2) is `max(scheduled_backoff, retry_after_seconds)` — the loop must never re-send while a rate-limit freeze (§7) is in effect, even if the fixed backoff schedule would otherwise allow it sooner.

### 6.4 Exhaustion

If `determine_reconcile_action()` returns `EscalateToOperator` (i.e. `query_attempts` has reached `kMaxQueryAttempts` = 3, all `Inconclusive`), §6.2's loop transitions to `EscalatedToOperator` — **this is "we don't know, a human decides," never "the order was confirmed absent."** No new logic is needed in `order_lifecycle.hpp`'s decision function for this; it already resolves correctly given a correctly-incremented, durably-persisted `query_attempts`.

### 6.5 Transition on `Found` — fixes round-3 P0 (a real safety bug, not just missing detail)

Round 2 said a `Found` result transitions the record to `Reconciled` — but `Reconciled` is one of `order_lifecycle.hpp::is_terminal()`'s terminal states. If Binance's confirmed status is `NEW` or `PARTIALLY_FILLED`, the order is **still open on the exchange** — marking it terminal would make the system stop tracking a live position, which is a genuine correctness/safety bug, not a modeling nicety.

Fix: `order_lifecycle.hpp::validate_transition()`'s `Ambiguous` case is extended (proposed change to existing code) to allow transitioning into whichever state matches Binance's confirmed status, not force everything through `Reconciled`:

```cpp
// Proposed change to order_lifecycle.hpp::validate_transition()'s Ambiguous case:
case OrderState::Ambiguous:
    if (to == OrderState::Accepted ||     // confirmed NEW — still live, resting
        to == OrderState::PartialFill ||  // confirmed PARTIALLY_FILLED — still live
        to == OrderState::Filled ||       // confirmed FILLED — terminal
        to == OrderState::Rejected ||     // confirmed REJECTED — terminal
        to == OrderState::Cancelled ||    // confirmed CANCELED — terminal
        to == OrderState::Expired ||      // confirmed EXPIRED — terminal
        to == OrderState::EscalatedToOperator)  // §6.4 — exhausted retries
        return TransitionResult::Ok;
    break;
```

`apply_confirmed_state()` (§6.2's pseudocode) maps `ReconcileQueryResult::confirmed_state` to the matching `OrderState` and calls `transition_to()` with it — **in-flight release happens only when the resulting state is terminal** (`is_terminal()` already correctly excludes `Accepted`/`PartialFill`, so this requires no change to `is_terminal()` itself, only to which states `Ambiguous` may transition into). A live, resting order found via reconciliation stays registered and continues to be tracked exactly as if it had been accepted through the non-ambiguous path.

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

### 7.1 Order-count needs one tracker per interval, not "a second tracker" (fixes round-3 P1)

Round 2 proposed "a second, distinctly-configured instance" for order-count — but Binance reports order-count across **multiple simultaneous intervals** (e.g. `X-MBX-ORDER-COUNT-10S` *and* `X-MBX-ORDER-COUNT-1D` in the same response), each an independent limit. A single second tracker can only represent one window; it can't represent both at once.

```cpp
// A small, fixed-capacity set — no heap allocation, matching this codebase's
// engineering discipline. Bounded to Binance's actual published interval
// count (currently well under 8); intervals are discovered from whichever
// X-MBX-ORDER-COUNT-* headers are actually present in a response, not
// hardcoded to a specific set that could go stale if Binance adds one.
class OrderCountTrackerSet {
public:
    // Parses the interval suffix (e.g. "10S", "1D") from the header name and
    // corrects that interval's tracker — creating a slot for a
    // never-before-seen interval if capacity allows, otherwise fail closed
    // (treat as if that interval were exhausted; never silently drop it).
    void correct_from_header(std::string_view interval_suffix,
                              std::uint32_t used_count,
                              TimePoint now = Clock::now()) noexcept;

    // ALL known intervals must independently permit sending — one exhausted
    // bucket blocks the send even if every other interval has headroom.
    bool can_send_all(TimePoint now = Clock::now()) const noexcept;

    // A 429/418 freezes every interval bucket this set knows about, not just
    // the one whose header happened to be read most recently — Binance's
    // rate-limit response doesn't indicate which specific bucket triggered
    // it, so the conservative response is to freeze all of them.
    void freeze_all_until(TimePoint until) noexcept;

private:
    static constexpr std::size_t kMaxIntervals = 8;
    struct IntervalTracker {
        bool active{false};
        char interval_suffix[8]{};       // "10S", "1D", etc.
        RequestWeightTracker tracker{};  // reused as-is per interval
    };
    std::array<IntervalTracker, kMaxIntervals> intervals_{};
};
```

### 7.2 Orchestration wiring

One `RequestWeightTracker` instance tracks request weight (as today, unchanged); one `OrderCountTrackerSet` tracks order-count across all its reported intervals. **Both** the weight tracker's `can_send()` and the order-count set's `can_send_all()` must pass (L5 spec's Gate 8) before a submit attempt proceeds. A `429`/`418` calls **both** `RequestWeightTracker::freeze_until()` *and* `OrderCountTrackerSet::freeze_all_until()` — the response doesn't indicate which bucket triggered it, so the conservative response is to freeze everything until `Retry-After` elapses.

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
