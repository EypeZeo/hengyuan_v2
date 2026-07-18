# Binance Private REST — L4 Foundation Spec (Draft, Not Implemented)

## Status

**Draft.** No code from this spec has been written. Per `docs/NATIVE_ARCHITECTURE.md`'s gate table, everything in this spec is **L4** — signed *read-only* requests. Nothing here submits an order; `docs/SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` (L5, POST) depends on this spec's primitives and must not be implemented before this one is accepted.

This spec exists because the original L5-only draft was rejected on review (see that file's changelog) for treating reconciliation, environment binding, signing discipline, and rate-limit accounting as someone else's problem "for later." They aren't — they're the shared foundation both reconciliation-after-ambiguous and the POST adapter need, and per ADR-019 D8, reconciliation specifically cannot be deferred past the first live submit path.

## 1. Environment binding (fixes: testnet unreachable in the original draft)

The original draft defaulted to `api.binance.com` with no testnet in the allowlist, while separately requiring a testnet run first — self-contradictory. Fix:

```cpp
enum class BinanceEnvironment : std::uint8_t {
    Testnet = 0,
    Production = 1,
};

struct EnvironmentBinding {
    BinanceEnvironment environment;
    std::string_view base_host;           // "testnet.binance.vision" or "api.binance.com"
    TransportPolicy transport_policy;     // allowlist scoped to ONLY this environment's host(s)
    std::string_view credential_env_key;  // e.g. HENGYUAN_BINANCE_TESTNET_SECRET vs
                                           // HENGYUAN_BINANCE_LIVE_SECRET — distinct names so
                                           // a testnet key can never be silently loaded where a
                                           // production key was expected, or vice versa
};
```

Rules:

- `EnvironmentBinding` is chosen once at process startup and is **immutable** for the process lifetime — no setter, no runtime flag to flip it later.
- Testnet's `TransportPolicy::endpoint_allowlist` contains only `testnet.binance.vision`; production's contains only `api.binance.com` + `api1/2/3.binance.com`. **Never share an allowlist between environments** — this is the concrete mechanism that makes "dry-run accidentally pointing at production" structurally impossible rather than a documentation promise.
- The credential env-var name is environment-specific so `env_loader.hpp`'s allowlist (`EnvAllowlist`) naturally rejects a testnet key presented under the production variable name, and vice versa.

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

Fixes: SubmitPort's ABI only carries `symbol_id` + integer ticks, but Binance's wire format needs a `symbol` string and decimal `price`/`quantity` strings matching exchange filter precision. This section defines the missing, immutable mapping.

```cpp
struct SymbolRegistryEntry {
    char symbol[12]{};           // e.g. "BTCUSDT"
    std::uint8_t price_scale{};  // decimal places implied by PRICE_FILTER.tickSize
    std::uint8_t qty_scale{};    // decimal places implied by LOT_SIZE.stepSize
    std::uint32_t rules_version{}; // monotonically increasing, bumped on every registry refresh
};
```

Rules:

- Populated once at startup (and on an explicit, operator-triggered refresh — never silently mid-session) from `exchangeInfo`.
- `rules_version` must be cross-checked between `account_truth.hpp`'s `validate_pre_trade()` (which already carries `SymbolRules`) and whatever later constructs the wire request: **if the registry version used for pre-trade validation doesn't match the version used to format the request, fail closed** rather than submit an order sized under filter assumptions that may no longer hold.
- **Lossless tick→decimal formatting**: converting an integer tick count back to Binance's expected decimal string must use integer arithmetic (division/modulo against `10^price_scale` / `10^qty_scale`), never a floating-point round-trip — matching this codebase's existing `fixed_point.hpp` discipline. No scientific notation, no silently-dropped trailing precision.

## 6. `GET /api/v3/order` (signed, reconciliation query) — fixes: reconciliation was explicitly out of scope, must not be

ADR-019 D8 requires reconciliation-after-ambiguous to exist before any live POST path is authorized. The original draft excluded it "for later"; that's the review's first blocking finding, and it's fixed here.

```
GET /api/v3/order?symbol={symbol}&origClientOrderId={coid}&recvWindow={ms}&timestamp={now}&signature={...}
```

- Same signing pipeline as everything else in this spec (§2).
- **Memory→Database visibility lag**: Binance's REST query layer can transiently report `-2013 Order does not exist` even when the order was in fact accepted by the matching engine moments earlier, under load. This is a documented characteristic, not a bug to route around by assuming the first negative answer is authoritative.
- **Retry policy**: on `-2013`, retry the *same* query (not the original POST — that's still forbidden) up to `order_lifecycle.hpp::OrderRecord::kMaxQueryAttempts` (already defined as 3) with backoff (1s, 2s, 4s). This is the network call that `order_lifecycle.hpp::determine_reconcile_action()`'s `QueryOrder` action already models in the state machine — this section defines what actually executes when that action fires.
- If still inconclusive after 3 attempts, the existing state machine already resolves to `EscalateToOperator` — no new logic needed there, just this network implementation feeding it real answers instead of a mock.

## 7. Rate-limit header accounting (fixes: local-only weight=1 assumption)

After **every** response (success or error), parse:

- `X-MBX-USED-WEIGHT-*` (interval-suffixed, e.g. `X-MBX-USED-WEIGHT-1M`) — Binance's authoritative view of request-weight consumption.
- `X-MBX-ORDER-COUNT-*` (e.g. `X-MBX-ORDER-COUNT-10S`, `X-MBX-ORDER-COUNT-1D`) — **a separate rate-limit bucket from request weight**, specific to order-placing endpoints.
- `Retry-After` (present on `429`/`418` responses, seconds) — see §8's response-mapping for how this feeds the freeze behavior.

Feed the actual `X-MBX-USED-WEIGHT-*` value back into `RequestWeightTracker` (`transport_policy.hpp`) after each response, correcting any drift from the local static per-call estimate (currently a hardcoded `order_weight` field) — the local tracker is a *pre-check* to fail fast, not the source of truth; Binance's own headers are.

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
