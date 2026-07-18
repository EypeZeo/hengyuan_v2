# Real `SubmitPort` Implementation — Design Spec (Draft, Not Implemented)

## Status

**Draft.** This is a design proposal only — no code from this spec has been written. Per `docs/DELEGATION_POLICY.md` Level 3 (live trading), this should go through a spec-approval pass (Architect role) before any implementation begins, even in v2's lighter process. Writing this spec is not the same as approving it.

## Goal

Define how `live_submit_orchestrator.hpp`'s `SubmitPort` — currently a function-pointer injection point satisfied only by test mocks — would be backed by a real, authenticated `POST /api/v3/order` call to Binance, reusing existing components rather than inventing new ones.

## What already exists and would be reused as-is

| Component | File | Role in this design |
|---|---|---|
| HMAC-SHA256 signing | `binance_signer.hpp` | Already correct, verified against Binance's own test vector. Pre-hashed key, zero-alloc signing. Would sign the request's query string exactly as it already does. |
| Transport policy | `transport_policy.hpp` | Endpoint allowlist, TLS verification, redirect rejection, response-size limit, rate limiting (post-F7/F8 fixes), clock-skew check, error sanitization — all already implemented and tested. Would gate every outbound call. |
| Boost.Beast SSL REST pattern | `binance_rest_snapshot.hpp` | The synchronous HTTPS GET pattern (resolve → connect → TLS handshake → write → read → parse) is the right shape for a POST too; this design extends it rather than replacing it. |
| Secure credential loading | `env_loader.hpp` | Already loads real `.env` credentials with permission/symlink checks, mlock, secure wipe. The real API key/secret would come from here, never from CLI args or hardcoded values. |
| Idempotency / order lifecycle | `order_lifecycle.hpp`, `InFlightRegistry` | Already generates the `newClientOrderId` and guards against duplicate submission. This design does not change that — it only fills in what happens *after* those checks pass. |

## What's new in this design

### 1. Request construction

```
POST https://api.binance.com/api/v3/order
Query string (order matters for signing, but Binance doesn't require a specific order —
what matters is the signature covers exactly this string):
  symbol={symbol}&side={BUY|SELL}&type=LIMIT&timeInForce=GTC
  &quantity={qty}&price={price}&newClientOrderId={coid}
  &recvWindow={ms}&timestamp={now_ms}
  &signature={hmac_sha256_hex(query_string_without_signature)}
Header: X-MBX-APIKEY: {api_key}
```

`OrderSide`/`OrderType` (already added to `account_truth.hpp` in the F1 fix) map directly to the `side`/`type` query params — `OrderType` is frozen to `Limit` so `type=LIMIT` is currently the only value this would ever emit.

### 2. New component: `binance_private_rest.hpp` (not yet written)

Proposed shape:

```cpp
struct PrivateRestConfig {
    std::string host = "api.binance.com";
    std::string port = "443";
    std::uint32_t connect_timeout_ms = 5000;
    std::uint32_t read_timeout_ms = 10000;
};

class BinancePrivateRestClient {
public:
    // api_key is non-secret (goes in a header, not signed); signer must
    // already be init()'d with the API secret via SecureEnvLoader.
    bool init(std::string_view api_key, BinanceSigner& signer,
              const TransportPolicy& policy) noexcept;

    // Blocking. Called only from the L5 runtime thread that owns SubmitPort,
    // never from the hot path. Returns SubmitResponse matching the ABI
    // live_submit_orchestrator.hpp already defines.
    SubmitResponse submit_order(
        std::string_view client_order_id,
        std::uint32_t symbol_id, OrderSide side, OrderType type,
        std::int64_t price_ticks, std::int64_t qty_ticks) noexcept;

private:
    // ... Boost.Beast SSL stream, reused pattern from binance_rest_snapshot.hpp
};
```

### 3. Gates checked before every send (all already exist, this design wires them in order)

1. `transport_policy::validate_policy()` — TLS/timeout/allowlist sanity, once at startup.
2. `transport_policy::check_endpoint()` — the resolved host must be in the allowlist.
3. `transport_policy::check_clock_skew()` — against Binance's `GET /api/v3/time` (would need a small addition: a cached server-time offset, refreshed periodically, not fetched per-order).
4. Sign via `BinanceSigner::sign()`.
5. Send via Beast, honoring `connect_timeout_ms`/`read_timeout_ms`.
6. `transport_policy::check_http_status()` — reject 3xx (no auto-follow).
7. `transport_policy::check_response_size()` — cap response body size.
8. Parse response JSON via simdjson (same library already used in `binance_rest_snapshot.hpp`).
9. On any transport-level failure (timeout, connection reset, TLS failure) → `SubmitOutcome::NetworkError`, which the orchestrator already routes to `Ambiguous` (M6 — no blind retry, must reconcile via `GET /api/v3/order` with the same `clientOrderId`).

### 4. Response mapping

| Binance response | `SubmitOutcome` |
|---|---|
| HTTP 200, `status: NEW\|FILLED\|PARTIALLY_FILLED` | `Accepted` (with `exchange_order_id` from `orderId`) |
| HTTP 400 with a Binance error code (e.g. `-1013`, `-2010`) | `Rejected` (with `error_code` from `sanitize_error()`) |
| Connect/read timeout | `Timeout` |
| DNS failure, connection reset, TLS handshake failure | `NetworkError` |

### 5. What this design deliberately does NOT do

- Does not implement `GET /api/v3/order` reconciliation (the query-after-ambiguous path) — that's `order_lifecycle.hpp`'s `determine_reconcile_action()` territory and would need its own small `binance_private_rest.hpp` addition, not covered here.
- Does not add real testnet/production endpoint switching logic — that's an explicit config decision the operator makes, not something to auto-detect.
- Does not touch `AuditRingSink` — still in-memory (F5 fix made it fail-closed when full, but persistent storage is a separate, larger change).
- Does not implement or propose an implementation timeline. This spec exists so that *when* implementation is authorized, there's a concrete shape to review rather than starting from a blank page.

## Before implementation begins (carried over from the L5 review's outstanding checklist)

1. This spec itself needs Architect review/acceptance.
2. A real dry-run harness (item 5 in the D12-9 checklist) exercising this shape against Binance **testnet** first — requires the owner's testnet API key, not production credentials.
3. Independent Opus review of the actual implementation once written, per `docs/DELEGATION_POLICY.md` Level 3.
4. The owner's own explicit decision to authorize it for anything beyond testnet.
