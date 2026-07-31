# Real `SubmitPort` Implementation — L5 POST Adapter Spec (Draft, Not Implemented)

## Status

**Draft, revision 73.** No code from this spec has been written — **acceptance closes findings at the spec level only**; implementation-level closure is defined exclusively by the shared fault-injection matrix (end of this file / L4) passing against real code. The existing `native/` sources still implement the pre-revision model (no `Submitting → PartialFill`, no `Accepted`/`PartialFill` → `Cancelled`, no `EscalatedLedger`, no extended `OrderRecord` poll fields) — **nothing in this document may be claimed as compile-time or implementation-level P0 closure**. This is **L5** (per `docs/NATIVE_ARCHITECTURE.md`'s gate table — signed order-*submission*, the highest gate in this repo), and it **depends entirely on `docs/BINANCE_PRIVATE_REST_L4_SPEC.md`** being accepted first.

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

### Revision 5 changelog (Architect review round 4, rejected — revision required)

Round 4 found the `rules_version` fix from round 3 had the identical class of self-contradiction round 3 found in `RateLimited` (a "before `Submitting`" claim the actual gate order didn't support), plus two more incomplete-wiring P0s and three P1s. All verified against actual code/spec text before fixing:

- **P0 (self-contradiction, same class as round 3's `RateLimited`)**: `StaleRulesVersion`'s comparison was specified as living inside `submit_order()` — but `submit_order()` is only reached via `SubmitPort::call()`, which §3's own gate list places *after* Submitting/in-flight registration/audit-submitted-ack. Fixed (§2.1/§2.2): the version check is now its own gate, run directly against a new `SubmitPort::CurrentRulesVersionFn` — a pure local comparison requiring no network call — positioned genuinely first in §3's list, before any audit write or state mutation.
- **P0**: `DurableAuditSink::recovery_scan(InFlightRegistry&)` could only repopulate bare registry membership, with no way to carry `query_attempts` (needed by `BINANCE_PRIVATE_REST_L4_SPEC.md` §6.2's fix) or fill data recorded before a crash. Fixed in §6.1: a new `RecoveredOrderRecord` struct and a fixed-capacity `std::array` output-buffer signature, mirroring `OrderRecord`'s own fields.
- **P0 (cross-file)**: L4 §6.1's `ReconcileQueryResult` had no fill-data fields, so a `Found` result confirming a fill had nothing to populate `OrderRecord::filled_qty_ticks`/`avg_fill_price_ticks` with. Fixed in L4 §6.1/§6.1.1.
- **P0 (cross-file)**: L4 §6.2's reconciliation-loop pseudocode called `append_durable()` three times without checking the return value — exactly the bug this spec's own §6.2 fixed for the submit path in round 3, missed on the reconciliation path. Fixed in L4 §6.2.
- **P1**: `OrderCountTrackerSet::correct_from_header()` returned `void` with no testable capacity-exhausted path, and nothing configured the actual per-interval limit (headers report used count only). Fixed in L4 §7.1.
- **P1**: `-1021` on the POST path was described the same as L4's read-only-GET handling ("pre-send correction") — but it's a response to an already-sent, already-Submitting, already-in-flight order, the same post-send-timing error round 3 found in `RateLimited`. Fixed in new §4.5: a single audited retry with the same `newClientOrderId`, re-entering the gate chain from the sign step (not from the top), falling through to `Ambiguous` on any second failure.
- **P1**: `GET /api/v3/account`'s handling was a one-line "populates `AccountSnapshot`," with no schema/conversion/capacity contract. Fixed in L4 §4.1–4.4.

### Revision 6 changelog (Architect review round 5, rejected — revision required)

Round 5 dug past spec-completeness into the actual implementation-level correctness of what round 4 shipped, finding genuine bugs verified against the real `transport_policy.hpp`/`account_truth.hpp` source (not just gaps in the spec text), plus a TOCTOU round 4's fix narrowed but didn't close, plus several stability/IO findings from an explicit request to check memory/IO/network/performance/stability. 6 P0s and 5 P1s, all fixed:

- **P0 (real shipped bug)**: `OrderCountTrackerSet` reuses `RequestWeightTracker` "as-is" per interval (round 3), but `RequestWeightTracker`'s actual source hardcodes a 60-second window — silently wrong for every interval except one that happens to be 60s (`1D` would fail-open after one real minute). Fixed in L4 §7.1: `RequestWeightTracker::reset()` gains a `window_seconds` parameter (proposed change to the existing class), and a new `parse_interval_seconds()` derives each interval's real window.
- **P0 (real shipped bug)**: `account_truth.hpp::checked_notional()`'s own comment says "caller must ensure same scale" — `validate_pre_trade()` never does, comparing the raw `price_ticks*qty_ticks` product (native scale = `price_scale+qty_scale`) directly against both exposure caps AND `AssetBalance::free_ticks` (fixed 8-decimal scale), two different scale domains sharing one variable. Fixed in L4 §5.1.1: a `rescale_notional_ceil()` helper normalizes to `kBalanceScale` once, with an explicit conservative rounding rule, before any comparison.
- **P0 (TOCTOU, not fully closed by round 4)**: comparing `current_rules_version()` against a remembered integer at Gate 1 only proves the versions matched at that instant — nothing stopped `submit_order()`'s wire-formatting step, gates later, from independently re-querying the registry and picking up a newer entry. Fixed in L4 §5.3 / this spec's §2/§2.1/§2.2: the orchestrator captures the actual `SymbolRules` value once and carries that snapshot (not a version integer) all the way to `submit_order()`, which performs no registry lookup of its own — Gate 1's comparison is now a fast-reject only, not the source of correctness.
- **P0**: `RecoveredOrderRecord` (round 4) still couldn't reconstruct enough to resume managing a recovered order (no side/type/intended price&qty/submit time/rules snapshot), `recovery_scan()` couldn't distinguish "nothing in flight" from "log corrupt/unreadable," and `AuditRecord` never carried what recovery needs to replay. Fixed in §6.1/§6.1.1/§6.1.2: extended `RecoveredOrderRecord`, a `RecoveryScanStatus` enum with explicit fail-closed Corrupt/CapacityExceeded/IoError cases plus a record-framing/checksum/truncated-tail contract, and 4 new `AuditRecord` fields (`side`, `order_type`, `resulting_state`, `rules_version`) plus a new `SymbolRegistryRefreshed` audit event.
- **P0 (cross-file, unverified-assumption error)**: HTTP 403 was classified as definitive `Rejected` ("blocked before the matching engine") on no actual Binance documentation guaranteeing that — the same class of error round 3 found in the original `RateLimited` framing. Fixed in §4.4: 403 now routes through `Ambiguous`.
- **P0 (cross-file)**: L4 §6.1's `Found` outcome was only checked against a matching `origClientOrderId`, the same gap round 3 fixed for the direct POST response (§4.2) but missed for reconciliation. Fixed in L4 §6.1.2 with the same two-step schema+field-match discipline.
- **P1**: `-1021`/`429`/`418`'s handling all assumed a well-formed `Retry-After` header; missing/malformed/negative/zero/absurd values were undefined at every call site. Fixed in L4 §7.3 with `parse_retry_after()` and explicit fail-closed defaults, referenced from §4.4's routing table below.
- **P1 (real gap, from the explicit stability/IO review request)**: the response-size cap was checked only *after* the existing `binance_rest_snapshot.hpp` read pattern had already buffered the full body into memory — too late to prevent the allocation it exists to cap. Fixed in L4 §8: `body_limit()` configured on the parser before reading.
- **P1 (real gap, same review request)**: `submit_order()`/`query_order()` are declared `noexcept` but the Beast/Asio/OpenSSL/simdjson machinery beneath them can throw, which would call `std::terminate()` on an uncaught exception. Fixed in L4 §8: both must wrap their bodies in try/catch, matching `binance_rest_snapshot.hpp`'s own existing pattern, converting any exception to `NetworkError`/`Inconclusive`.
- **P1**: no revision had defined who owns the shared mutable state (`RequestWeightTracker`, `OrderCountTrackerSet`, `SymbolRegistry`, `DurableAuditSink`) across submit/reconciliation/account-refresh/registry-refresh, none of which is internally synchronized. Fixed in L4 §9: single-owner-thread model, with `SymbolRegistry`'s existing `std::shared_mutex` kept as the one deliberate exception for the operator-refresh thread boundary.
- **P1**: `FULL` response validation's required-field list and cross-check omitted `origQty`/`cummulativeQuoteQty`/`timeInForce`/`type`, and never actually specified that `executedQty`/`cummulativeQuoteQty` (not `fills[]`) are the fill-data source. Fixed in §4.2.
- **P1**: this file's own `GET /api/v3/order` example (L4 §6) used `symbol&origClientOrderId&recvWindow&timestamp`, not alphabetical — contradicting L4 §2.1's own canonical-order rule. Fixed in L4 §6.

### Revision 7 changelog (Architect review round 6, rejected — revision required)

Round 6 found the most severe self-inflicted bug of any round so far: revision 6's own §6.1.2 fix named a Binance response field that doesn't exist, which would have made reconciliation's `Found` outcome permanently unreachable. Round 6 also found real dimensional-analysis and overflow bugs in round 5's own "fixed" arithmetic, and dug (per an explicit request to hunt for hidden bugs and check memory/IO/network/performance/stability) into several gaps neither this spec nor L4 had addressed at all. 8 P0s and 8 P1s, all fixed:

- **P0 (self-inflicted, most severe)**: L4 §6.1.2's round-5 fix required `GET /api/v3/order`'s **response** to contain `origClientOrderId` — that's the **request** parameter name; the response identifies the order via `clientOrderId`. Every legitimate response would fail schema validation → `Inconclusive` → escalate, always. Fixed in L4 §6.1.2: corrected field name; `query_order()` (§2 above) now takes a full `OrderExpectation` instead of a bare ID, since checking side/type/price/qty against the response (round 5's own stated goal) was never actually possible with the old signature — `OrderRecord` gains the fields needed to build one.
- **P0**: `-2010` was on §4.4.1's `Rejected` allowlist; Binance documents it as covering duplicate-`clientOrderId` cases, where the order might actually already be live. Fixed: removed from the allowlist, defaults to `Ambiguous`.
- **P0 (dimensional-analysis bug)**: L4 §6.1.1's `avg_fill_price_ticks` formula passed only `price_scale` into a generic division helper, silently assuming `cummulativeQuoteQty` was already scaled at `price_scale` — it's actually scaled at a separate `quote_scale` (new `SymbolRules` field, this revision). Fixed with the correct derivation (`price_scale + qty_scale - quote_scale` as a signed exponent) and a checked helper (used identically here and in §4.2 below) that handles it without floating point or silent overflow.
- **P0 (overflow bugs in round 5's own "fixed" code)**: L4 §5.1.1's `rescale_notional_ceil()` had an unbounded `pow10_i64()` (10^19 overflows int64_t, uncapped) and a ceiling-division idiom (`raw + div - 1`) that itself overflows for `raw` near `INT64_MAX`. Fixed with a bounded `pow10_i64()` (rejects delta >= 19) and a quotient/remainder ceiling with no addition-overflow path.
- **P0**: `RecoveredOrderRecord`'s promised full-order recovery had nowhere to put a registry-wide snapshot inside the fixed-size `AuditRecord`. Fixed in §6.1.1/§6.1.2 with a second, variable-length `SymbolRegistrySnapshot` durable-record type, distinguished by a new `DurableRecordType` byte.
- **P0**: §6.1.1's "discard the last record on checksum failure" rule conflated a torn write (safe to discard) with a complete-but-corrupted record (never safe, even if last) — a corrupted, already-`Acked` `OrderSubmitted` could be silently dropped, risking a duplicate submission on recovery. Fixed: only a record that's not fully physically present is discarded; any complete frame with a checksum mismatch is `Corrupt`, regardless of position.
- **P0 (new gap, not previously addressed by any round)**: no revision required the actual `Accepted`/`Rejected`/`Filled`/`PartialFill` outcome to be durably ACKed before releasing in-flight/transitioning state — only the pre-send intent/submitted writes were covered. Fixed in new §6.4: the same durable-ACK-before-consequential-action discipline §6.2 already applies pre-send, applied here post-response.
- **P0 (new gap)**: `OrderCountTrackerSet::can_send_all()` was a pure, non-reserving check, and a fresh process's trackers assumed zero prior usage even for account-wide, restart-surviving intervals (`1D`) — a real fail-open. Fixed in L4 §7.1/§7.1.1: `try_reserve_all()` (reserve-before-send, never rolled back on a later network failure) plus an explicit startup-baseline rule that blocks long intervals until a real response header confirms actual usage.
- **P1**: long intervals modeled as a purely local relative sliding window, drifting from Binance's actual (likely UTC-boundary-aligned) reset schedule over a full day. Refined in L4 §7.1 to anchor long-interval windows to server-time-aligned boundaries.
- **P1**: `Retry-After` values over 1 hour were clamped and then actively retried — unsafe against Binance's documented multi-day 418 ban durations. Fixed in L4 §7.3: an absolute, unclamped 64-bit deadline; permanent fail-closed only on the (practically unreachable) unrepresentable case.
- **P1**: only `NEW`/`PARTIALLY_FILLED`/`FILLED` were named; `PENDING_CANCEL`/`EXPIRED_IN_MATCH`/unrecognized statuses had no defined handling. Fixed in L4 §6.5 with an exhaustive status table (unrecognized → `Inconclusive`, never a guess), plus HTTP 401/404/405/409/any-unlisted-status rows added to §4.4 below.
- **P1**: `PrivateRestConfig` (§2 above) had only 2 timeout fields despite L4 §8 already claiming 5 separate per-phase deadlines existed. Fixed: 5 explicit fields, plus an explicit cancel-and-never-reuse-the-connection rule for any phase that times out.
- **P1**: the single-owner thread (round 5) would starve control-plane/clock-sync/account-refresh work if implemented as a blocking loop with inline backoff sleeps. Fixed in L4 §9.1 with an actor/scheduled-task model, control-plane-priority servicing. §9.2 also fixes previously-undefined cross-thread publish semantics for `current_rules_version()` (routed through the same registry lock) and the clock-offset resync (a single `std::atomic<int64_t>`).
- **P1**: `body_limit()` (round 5) covered only the response body; header-size limit, decompression-bomb exposure, and force-close-after-exception were all unaddressed. Fixed in L4 §8.
- **P1**: the durable log's byte format had no defined endianness/padding/enum-range-validation/version-migration/single-writer discipline/flush-deadline, and its CRC32C checksum provided corruption detection but no tamper evidence. Fixed in new §6.1.1.1: little-endian explicit field encoding (no raw struct `memcpy`), a version-byte dispatch with an `IoError` path for unrecognized versions, enum range-checking on every decode, an OS-level single-writer file lock, a bounded flush deadline, and an HMAC-SHA256 checksum (keyed via a value derived from, but distinct from, the existing Binance API secret) in place of a bare CRC.

### Revision 8 changelog (Architect review round 7, rejected — revision required)

Round 7 moved past spec-completeness and dimensional/overflow bugs into failure-injection scenarios this design hadn't been tested against on paper: deliberate frame deletion, crash loops around reconciliation's own attempt counter, and a self-contradictory state left by round 6's own audit-failure fallback. It also caught two factual errors — one about Binance's actual `rateLimitType` values, one about a startup-baseline endpoint that does in fact exist. 8 P0s and 8 P1s, all fixed:

- **P0 (most severe, tamper-evidence gap)**: round 6's HMAC covered only `length + payload` — `version`/`type` were unauthenticated, and each frame's MAC being independent of every other frame's meant a whole valid frame (e.g. an already-`Acked` `OrderSubmitted`) could be deleted from the log with every *remaining* frame still verifying individually. Fixed in new §6.1.1.1: full-frame MACs, a monotonic-sequence hash chain (each frame's MAC covers the previous frame's MAC), and a persistent chain-tip anchor stored outside the append-only log to detect trailing-frame deletion too.
- **P0**: `SymbolRegistry::refresh_from_exchange_info()` (L4 §5.3) never specified whether publishing the new `rules_version` in memory happens before or after the corresponding `SymbolRegistrySnapshot` frame is durably ACKed — an order could reference a version with no durable snapshot to recover it from. Fixed in L4 §5.3.1: the durable write is now a precondition of publication.
- **P0**: the variable-length `SymbolRegistrySnapshot` frame (round 6) had no `symbol_count` bound, no checked-offset-arithmetic requirement, and no bound on the recovery-time rules_version index. Fixed in new §6.1.3: capped at `kMaxSymbols`, no heap allocation, checked arithmetic throughout, a bounded version-index.
- **P0 (self-contradiction)**: §6.4's post-response-audit-failure fallback (round 6) left the order at `Submitting` — but `order_lifecycle.hpp::determine_reconcile_action()` only acts on `Ambiguous`; a record left at `Submitting` would never be picked up by reconciliation again, not on a live crash-free continuation and not after a restart (recovery would restore the same `Submitting` state). Fixed: the in-process fallback now transitions to `Ambiguous`, and `recovery_scan()`'s caller contract (§6.1) explicitly remaps any recovered `Submitting` state to `Ambiguous` too — two halves of the same guarantee.
- **P0 (factual error)**: L4 §7.1's `configure_limit()` filtered `exchangeInfo`'s `rateLimits[]` for `rateLimitType=ORDER_COUNT` — Binance's actual value is `ORDERS`. The filter would match nothing, leaving the tracker set empty, and an empty set made `can_send_all()` **vacuously return true** (a real fail-open). Fixed: corrected the type name; an empty interval set now explicitly fails closed.
- **P0 (factual error)**: round 6 claimed no Binance endpoint reports order-count usage without consuming any, building a self-contradictory "block long intervals, but still allow a blind first send" rule on that false premise. `GET /api/v3/rateLimit/order` exists for exactly this. Fixed in L4 §7.1.1: L5 startup calls it and blocks entirely on failure.
- **P0 (ABI mismatch)**: `ReconcileQueryResult`/`SubmitResponse` still carried a `uint32_t retry_after_seconds` duration despite §7.3 (round 6) already producing an absolute deadline — reconstructing a deadline from a duration later, after the actor/scheduler's queueing delay, would reintroduce imprecision round 6 eliminated elsewhere. Fixed: both structs now carry the absolute deadline directly.
- **P0**: `RequestWeightTracker`'s side of Gate 8 still used the non-reserving `can_send()` (only `OrderCountTrackerSet` got round 6's reserving treatment), and §4.5's `-1021` retry re-checked non-reserving functions entirely, bypassing reservation on a genuine second send. Fixed in L4 §7.2: a single `try_reserve_all_budgets()` (weight + order-count, rollback-on-partial-failure) used at every order-placing send site, including the retry.
- **P1**: DNS-resolve cancellation wasn't actually guaranteed by `beast::tcp_stream`'s own timer; TLS minimum version/CA-store/cross-platform policy was a bare, unspecified bullet; reconciliation incremented+persisted `query_attempts` *after* the query rather than before, letting a crash-loop send unbounded real queries while the durably-reconstructed count never advanced. All three fixed (L4 §8, §6.2).
- **P1**: `query_order()`'s separate `symbol` argument could drift from the captured snapshot's own symbol; `timeInForce` was schema-required but never field-matched; replay had no defined handling for duplicate/out-of-order/contradictory events. Fixed: `query_order()` derives `symbol` from the snapshot exclusively (new §6.1.4), `timeInForce` is now field-matched (L4 §6.1.2), and replay walks `order_lifecycle.hpp::validate_transition()` itself rather than trusting the log's ordering.
- **P1**: `rescale_notional_ceil()`'s `std::uint8_t` scale parameters could silently narrow-wrap a corrupted `price_scale + qty_scale` sum before the function's own checks ever saw it. Fixed in L4 §5.1.1 with `int` parameters and an explicit range check.
- **P1**: clock-offset freshness was TTL-only — a wall-clock step change (NTP correction, VM pause/resume) within the TTL window would go undetected. Fixed in L4 §2.2 with a wall-clock/monotonic-clock consistency check performed at every signed-request preparation.
- **P1**: the audit-log HMAC key (round 6) had no defined identity or rotation story — rotating the underlying API secret would silently make the entire prior log unverifiable. Fixed in new §6.1.1.2: a `key_id` field per frame, a retained key-history mapping, and environment-bound key derivation.

### Revision 9 changelog (Architect review round 8, rejected — revision required)

Round 8 injected failure scenarios against the *fixed* revision-8 design and found seven still-reachable P0 windows (most of them "fixed on paper, still reachable after the fix"), plus several P1s and deeper bugs this revision also closes. Both files revised together — see L4 revision 8 for the cross-file half.

- **P0 (recovery false-Corrupt)**: §6.1.4's round-7 draft called `validate_transition()` on every event, including the first `OrderIntentCreated` (`Intent → Intent`) and idempotent same-state re-assertions (`Submitting → Submitting`). Both are `InvalidTransition` in real `order_lifecycle.hpp` — a normal restart would refuse to start. Fixed in §6.1.4: first event is an *establish*, same-state is an accepted no-op, only genuine different-state transitions consult the state machine.
- **P0 (cross-file)**: L4 §6.2's `schedule_reconcile_retry_at()` neither `return`ed nor `break`ed, so `while(true)` re-issued GET immediately and burned `kMaxQueryAttempts` without honoring backoff/`Retry-After`. Also used `?:` instead of the `max(...)` §6.3 already required. Fixed in L4 §6.2.
- **P0 (anchor gap)**: periodic chain-tip anchors left every frame after the last anchor unprotected — delete a trailing `OrderSubmitted`, remaining chain still matches the old tip. Missing/deleted anchor had no fail-closed rule. Fixed in §6.1.1.1: tip anchor is updated as part of every `Acked` append (same durability barrier); non-empty log with missing/unreadable anchor is `Corrupt`/`IoError`.
- **P0 (cross-file)**: 429/418 freeze lived only in in-memory `TimePoint`; restart mid-ban could immediately re-send. Fixed: durable `RateLimitFreeze` record + recovery restore (§6.1.2 / L4 §7.3.1), deadline stored as UTC wall-clock ms (steady_clock cannot survive restart).
- **P0 (cross-file)**: ORDERS (and REQUEST_WEIGHT) modeled as relative sliding windows; Binance uses server-aligned fixed buckets for *every* interval, and fills can decrement unfilled ORDERS count. Fixed in L4 §7.1: fixed-bucket rotation for all intervals; never locally decrement on assumed fills — only header / `rateLimit/order` may lower the count.
- **P0**: `DurableAuditSink` exposed only `append_durable(const AuditRecord&)`, so L4 §5.3.1's "ACK snapshot before publish" was unimplementable against the declared ABI. Fixed in §6.1: `append_snapshot(...)` (+ `append_rate_freeze(...)`) as first-class methods.
- **P0 (cross-file)**: request-weight reservation covered only `POST /api/v3/order`; `/account`, reconciliation query, startup `/rateLimit/order`, and `exchangeInfo` had no endpoint-weight table or pre-send `try_consume`. Fixed in L4 §7.4.
- **P0 (deeper, not in the review list)**: after `Found → Accepted`/`PartialFill`, `determine_reconcile_action()` returns `NoAction` — live resting orders would never be polled for fills again. Fixed in L4 §6.6 with an open-order status-poll schedule distinct from Ambiguous's escalate-at-3 path.
- **P1**: flush-deadline `Failed` had no fencing — a late `fsync` could still land bytes after the caller observed failure. Fixed in §6.1.1.1: fence the writer and stop L5.
- **P1**: `kMaxTrackedRegistryVersions=64` had no refresh cadence, compaction trigger, or "non-terminal orders pin their snapshot" rule. Fixed in §6.1.3.
- **P1**: this file's §5 still said reconciliation persists `query_attempts` *after* each attempt — contradicts L4 §6.2 (persist *before* GET). Fixed.
- **P1**: L5 §3 gate list never named joint reservation; easy to re-implement Gate 8 as bare `can_send()`. Fixed — explicit gate before sign/send.
- **P1 (cross-file)**: `RequestWeightTracker::reset()`'s proposed `window_seconds` as 3rd parameter would break `t.reset(limit, margin, t0)` call sites. Fixed in L4 §7: `window_seconds` is the 4th parameter (after `now`).
- **P1**: TLS/DNS/body limits remain as specified; mandatory fault-injection matrix added under "Before implementation begins."

### Revision 10 changelog (Architect review round 9, rejected — revision required)

Round 9 found nine still-reachable P0s (several structural: cyclic L4↔L5 dependency, ABI that cannot express the stated rules) plus P1s. Durable control-plane primitives (freeze/snapshot) sink into L4 §10; this file keeps order-event audit + recovery.

- **P0 (PartialFill path)**: §4.3 required `Submitting → Accepted → PartialFill` as one outcome record, but `validate_transition` forbids `Submitting → PartialFill` and a single frame cannot encode two transitions. Fixed: propose `Submitting → PartialFill` (symmetric to existing `Submitting → Filled`); one outcome record with `resulting_state=PartialFill` + fill fields; fill data carried on that frame.
- **P0 (OrderSubmitted audit failure)**: in-flight + `Submitting` already applied, append failure only `return`ed — slot leak / no compensation. Fixed in §6.2: deregister, transition to new terminal `AbortedPreSend`, best-effort durable abort record, fence L5 on sink failure.
- **P0 (compaction drops freeze)**: retain rule centered on in-flight orders could drop an active `RateLimitFreeze` from account/baseline with no orders. Fixed in §6.1.3: always retain every freeze with `deadline_utc_ms > now` and all permanent freezes.
- **P0 (cross-file)**: L4 freeze reverse-depended on L5 types — fixed by L4 §10 foundation; this file references L4, never the reverse.
- **P0 (cross-file)**: `append_snapshot` callable from operator thread — fixed in L4 §9: refresh is actor-enqueued only.
- **P0 (cross-file)**: fixed-bucket lacked `server_now_ms` — fixed in L4 §7.
- **P0 (cross-file)**: reservation-failure burned `query_attempts` — fixed in L4 §6.2 (reserve → persist → GET).
- **P0 (cross-file)**: `RAW_REQUESTS` + exclusive egress IP — fixed in L4 §7.5 / §1.1.
- **P1**: same-state replay now validates event_type↔state and fill-payload consistency; Intent-only orphans have an explicit recovery/capacity policy; compaction uses generation-atomic switch; HMAC key material under KEK + destruction rules (§6.1.1.2); host allowlist failover contract (§2 / L4 §1).

### Revision 11 changelog (Architect review round 10, rejected — revision required)

Round 10 found 8 P0s, several of them *collisions between round-9 fixes* (universal reservation vs. bootstrap data needs; strict same-state replay vs. progressive fills; per-second polling vs. the weight budget). All fixed; cross-file items in L4 revision 10:

- **P0 (progressive PartialFill → false `Corrupt`)**: round 9's replay rule (b) required same-state fill payloads to match the prior frame *exactly*, while §6.6 polling legitimately produces `PartialFill 0.1 → PartialFill 0.2` every second. Any partially-filling order that progressed would make its own recovery `Corrupt`. Fixed in §6.1.4: same-state fill re-assertions are legal iff fill fields are **monotonically non-decreasing** (and bounded by intended qty); live-path fill progress is a durable fill-progress frame + in-memory field update with **no** `transition_to()` call — no `PartialFill → PartialFill` self-transition needs to exist in the state machine, and none is added.
- **P0 (`-1021` self-contradiction)**: §4.5 asserted "the original send did not create a resting order" and allowed a same-COID re-POST — contradicting this spec's own §4.4 row calling `-1021` a post-send condition. A proxy/edge `-1021`-shaped 400, or any response whose provenance can't be proven, makes the re-POST a potential duplicate order. Fixed: §4.5 rewritten — forced clock resync for *future* requests, this order routes to `Ambiguous` + L4 §6 reconciliation, **no re-POST path exists** (`OrderClockResyncRetried` event retired; Gate 8 is now structurally the only order-placing send site).
- **P0 (poll scale)** / **P0 (bootstrap deadlock)** / **P0 (RAW restart baseline)** / **P0 (server-time error bound)** — fixed in L4 §6.6 / §7.0 / §7.5.1 / §7.1.2 respectively (this file's Gate list and §5 reference them).
- **P0 (audit rollback)**: local chain+anchor cannot detect whole-store rollback/deletion; fixed in L4 §10.2 — honest threat model + store-identity breadcrumb outside the store root + mandatory-for-production external tip anchor with bounded lag; delete-everything now recovers as `IoError`, never fresh `Clean`.
- **P0 (shared-type ODR)**: `AuditAppendResult`/`RecoveryScanStatus` "mirrored" here while L4 §10 called its own copies canonical — same-name enums in two headers. Fixed: single shared header `hengyuan/durable_control_plane.hpp` (L4 §10); §6.1 below *quotes* the L5-specific additions only and `#include`s the shared definitions.
- **P1 (gate order)**: §3's gate chain parsed the body schema before rate-limit headers/status — but a 429/418 body never satisfies the FULL success schema, so freeze handling sat behind a schema check doomed to fail first. Fixed: Gates 11–14 reordered — headers (incl. `Retry-After` + durable freeze) → HTTP status → size guard → schema, and §4.4's rows are evaluated in that order.
- **P1 (escalated orders exhaust capacity)**: `EscalatedToOperator` orders stayed in `InFlightRegistry` forever; 64 escalations would permanently zero submission capacity. Fixed in new §6.5: durably-ACKed escalations move to a fixed-capacity `EscalatedLedger` (COID-reuse still blocked; slot freed; operator resolution is a durable event; ledger overflow fails closed).
- **P1 (weight-config append ABI / store size caps / anchor-CURRENT replace contract)** — fixed in L4 §10/§10.1/§10.3.

### Revision 12 changelog (Architect review round 11, rejected — revision required)

Round 11 found that several round-10 "conservative" fixes still failed under torn reads, crash timing, time-base confusion, and contradictory control flow after persistence failure:

- **P0 (outcome audit fail vs fence contradiction)**: §6.4 on outcome-append `Failed` transitioned to `Ambiguous` and promised L4 §6.2 reconciliation GETs, while §6.1.1.1 said any `Failed` permanently fences the writer and stops durable-attempt GETs. Those two paths cannot both run. Fixed in §6.4: outcome-append `Failed` fences, keeps the order at `Submitting` in memory (recovery remaps to `Ambiguous` on the *next* process after storage repair), escalates operator, and issues **no** reconciliation GET in the fenced process.
- **P0 (`EscalatedLedger` recovery ABI incomplete)**: §6.5 prose claimed a second output array, but §6.1's `recovery_scan()` signature still only had the in-flight array — ledger/COID-reuse state was unrecoverable after restart. Fixed: `recovery_scan()` signature now takes both arrays + counts.
- **P0 (cross-file)**: clock-offset torn pair, header/bucket mismatch, crash-path bootstrap `/time`, Retry-After UTC/steady mix-up, open-poll failure crash amnesia, `OrderEvent=0` missing from enum, external-anchor export backpressure — fixed in L4 revision 11.
- **P1**: tip-anchor record carries `key_id` so post-rotation verification closes (§6.1.1.2); `read_timeout_ms` raised with an explicit matching-engine + network margin (§2); signer HMAC-only gate and production allowlist exclusions documented in L4 §2.3 / §1.

### Revision 13 changelog (Architect review round 12, rejected — revision required)

Round 12 found C++ memory-model UB, clock-identity overconfidence, unimplementable override ABI, and compaction/external-anchor continuity gaps:

- **P0 (`kMaxInFlight` dual definition)**: §6.1 used `kMaxInFlight`/`kMaxEscalated` before declaring them and re-declared `kMaxInFlight` despite `order_lifecycle.hpp:212` already owning it — ODR / declaration-order defect. Fixed: **sole** `kMaxInFlight` definition is `order_lifecycle.hpp`; this file `#include`s it and only defines `kMaxEscalated`.
- **P0 (compaction ↔ external anchor)**: generation switch lacked predecessor-MAC bridge and external-ACK-before-`CURRENT` order. Fixed in §6.1.3 + L4 §10.2 (`GenerationBridge` / `GenerationSeal`).
- **P0 (cross-file)**: clock publish UB, Unknown bucket identity, steady crash wait, 418/429 missing-Retry-After split, open-order observation matrix (`Accepted`/`PartialFill` → `Cancelled`, same-state no `transition_to`, cancel-failure reverse edges), OperatorOverride sidecar + `append_operator_override` + tip-MAC binding, incomplete CURRENT-flip recovery — fixed in L4 revision 12; this file's proposed `validate_transition` edges updated in §4.3.
- **P1**: poll-failure recovery by **sequence replay** (not "global max"); checkpoint fold; compaction chunk/yield; generation directory fsync order; hard-staleness blocks new submits (L4 §6.6 / §10.1 / §10.3).

### Revision 14 changelog (Architect repair round 13)

- **P0 (pre-send fact ambiguity)**: `OrderSubmitted` used to be ACKed before budget reservation and signing; any later local failure could leave a durable `Submitting` fact for a request that demonstrably never reached `async_write`. Fixed in §3/§6.2: reserve and format/sign first, then durably ACK a deliberately conservative `OrderSubmitPrepared` fact; any process death after that ACK is recovered as `Ambiguous` (never assumed unsent), while failures before it remain intent-only and are never reconciled.
- **P0 (late fsync)**: a `Failed` append may still have completed physically. The old compensation called that case `AbortedPreSend`; fixed by removing that unsafe claim. A failed prepared append fences/stops and leaves the COID tracked; next-process recovery treats any complete prepared/submitted frame as ambiguous.
- **P0 (checkpoint/replay)**: compaction now emits a self-contained `OrderRecoveryCheckpoint` record as the first retained record for each non-terminal COID. It carries immutable intent, rules reference, current state/fill/counters and contains no old-generation sequence reference; replay explicitly accepts it as an establish event.
- **P0 (generation seal)**: the external seal now commits the *final* compacted generation `{generation, final_seq, final_tip_mac, key_id, content_root}` after all retained frames are fsynced, not merely its bridge genesis. L4 defines authenticated external-anchor read semantics used by recovery.
- **P1**: poll staleness is checkpointed/restored; new audit event payloads and recovery fields are canonicalized in L4's shared durable header.

### Revision 15 changelog (Architect repair round 14)

- **P0 (self-inflicted ABI gap)**: `reset_after_seq` (§6.1.2) had no way to be obtained from `append_durable()`'s old `Acked`/`Failed`-only return. Fixed: `AuditAppendResult` (L4 §10) is now a struct with `.sequence`; every append comparison in this file updated to `.acked()`.
- **P0 (cross-file, self-contradiction)**: `OrderRecoveryCheckpoint::last_event_sequence` contradicted its own "no old-generation reference" claim; §6.1.4's replay loop read fields (`record.record_type`/`.checkpoint`) with no backing type. Fixed: renamed to `own_sequence`; new `DecodedOrderFrame` variant type defined and used throughout §6.1.4.
- **P0 (cross-file)**: compaction's snapshot-pin rule used `is_terminal()` (true for `EscalatedToOperator`), risking dropping a pinned snapshot an unresolved escalated order still needs. Fixed: new `is_exchange_final()`, used consistently in §6.1.3.
- **P0 (literal compile gap)**: this file's own header sketches use `SymbolRules`/`OrderSide`/`OrderType` throughout without including `account_truth.hpp`. Fixed.
- **P0/P1 (cross-file)**: L4 §7.3.1's freeze self-lock, §10.2's generation-vs-sequence rollback check, `GenerationBridgePayload` key lineage, `OperatorOverrideSidecar`'s trust-boundary documentation, `last_poll_completed_utc_ms` basis fix, `TimeResyncCredit` weight-config binding, `ClockOffsetPublisher`'s mutex default, `recover_control_plane()`'s `out_has_weights`, and `ExternalAnchorClient`'s "Acked" definition are all fixed in L4 revision 14 — see that file's changelog for the full detail.

### Revision 16 changelog (Architect repair round 15)

- **P0 (self-inflicted, cross-file)**: round 14's `OrderRecoveryCheckpoint::own_sequence` was a payload field requiring its own not-yet-assigned write sequence to already be known — circular, unconstructible under group commit. Fixed: field removed; the checkpoint's sequence comes from the frame header at decode time, and forward references use `AuditAppendResult.sequence` captured post-write.
- **P0 (self-inflicted)**: §6.1.4's same-state replay branch still called `advance_replayed_fill(replayed_fill, record)` (the raw `DecodedOrderFrame`) instead of `record.event`, inconsistent with the identical call in the adjacent branch — a leftover from round 14's type-safety fix. Fixed.
- **P0 (cross-file)**: L4 §7.3.1's `probe_server_time_during_freeze()` still depended on the ordinary weight trackers, reintroducing §7.0's bootstrap circularity for the freeze-recovery case and having no bound on retry frequency. Fixed in L4 with a dedicated `FreezeProbeCredit` — see that file's changelog.
- **P1**: `is_exchange_final()` (§6.1.3) omitted `AbortedPreSend`, a genuinely resolved state — every pre-send-aborted order would be checkpointed forever, an unbounded leak. Fixed: included.
- **P1 (cross-file)**: the local-append-vs-external-anchor linearization (thread roles, backpressure, timeout handling, the two distinct "Acked" meanings) was unspecified. Fixed in L4 §10.2.1.

### Revision 17 changelog (Architect repair round 16)

- **P0 (cross-file)**: L4 §10.2.1's durable-outbox gap and FIFO-ownership contradiction — see L4's revision 16 changelog for the full fix (reconstruct the export backlog from the local log at startup; sole FIFO ownership moved to a fixed-capacity SPSC `ExportOutboxRing` owned by the sink, not `ExternalAnchorClient`).
- **P0 (cross-file)**: L4 §7.3.1's `FreezeProbeCredit` had no backing durable record type and a hardcoded weight credit that could undercut a configured override — see L4's revision 16 changelog.
- **P0 (this file, dead/contradictory state)**: `OrderState::AbortedPreSend` was reachable via `Submitting → AbortedPreSend`, but §3's actual gate flow (as clarified this round) never leaves `OrderRecord::state` at `Submitting` for a pre-send failure — the `Intent → Submitting` transition is now explicitly defined as happening exactly once, at Gate 9, immediately after `OrderSubmitPrepared` is genuinely `.acked()` (not at Gate 6's in-flight registration, where an earlier revision's pseudocode comment placed it, and which was the actual root of the contradiction). Fixed: `AbortedPreSend` removed entirely — the enum value, its transition, `is_terminal()`'s inclusion of it, and `is_exchange_final()`'s now-unnecessary special-case for it (§6.1.3, reverted). §3 Gates 6–9 and §6.2's pseudocode both updated to state explicitly where `Submitting` is and isn't set. A stale "Gate 9's sign/format" cross-reference (should have read Gate 8) was also found and fixed while auditing this section.

### Revision 18 changelog (Architect repair round 17)

- **P0 (cross-file)**: L4 §7.3.1's `FreezeProbeCredit` durable-write path called the L5-only `DurableAuditSink::append_durable()` for an L4-canonical record, and `freeze_epoch` had no durable high-water mark surviving compaction/restart — see L4's revision 17 changelog for `append_freeze_probe_attempt()` and `FreezeEpochWatermark`.
- **P0 (cross-file)**: L4 §10.2.1's durable-outbox recovery had no actual bounded drain algorithm and never defined `ExportTuple` — see L4's revision 17 changelog.
- **P1 (this file)**: §6.1.3's compaction retain set never covered `FreezeProbeAttempt`/`FreezeEpochWatermark` records. Fixed: bullet 6, new — retain `FreezeProbeAttempt` records matching a retained freeze's epoch, always retain the single latest `FreezeEpochWatermark` regardless of whether any freeze is currently active.

### Revision 19 changelog (Architect repair round 18)

- **P0 (this file)**: the durable-outbox hard-lag age computation (L4 §10.2.1) had no uniform timestamp source across all frame payload types. Fixed in §6.1.1: `recorded_utc_ms` moved into the frame header itself (covered by the full-frame MAC), present on every frame type uniformly — `DecodedOrderFrame` (§6.1.4) gains the corresponding field, read from the header at decode time rather than any payload.
- **P0 (cross-file)**: L4 §10.2.1's unconditional "`read_latest_tip()` failure → L5 does not start" contradicted the `OperatorOverride` sidecar's existing admission path — see L4's revision 18 changelog for the normal/degraded path split.
- **P1 (cross-file)**: L4 §7.3.1's `freeze_epoch` increment had no overflow guard — see L4's revision 18 changelog.

### Revision 20 changelog (Architect repair round 19)

- **P0 (cross-file)**: L4 degraded sidecar path reset hard-lag to a fresh empty ring across crash→restart→new-override — see L4 revision 19 (`LastRemoteAckedTip` + ReadOnlyDrain).
- **P0 (this file + L4)**: mandatory server-corrected `recorded_utc_ms` in every frame header deadlocked Phase-B `unknown_time_429` freezes (no trusted UTC, forging forbidden). Fixed: header `FrameTimeKind` provenance; UnknownBootstrap frames use `recorded_utc_ms=0`, are MAC-covered, and are excluded from age/TTL (§6.1.1 / L4 §7.3.1).
- **P1 (this file)**: format-version bump + decoder/compaction migration for the new header fields (§6.1.1.1) — current write version = 3.
- **P1 (cross-file)**: `GenerationSeal` content-root normative byte list now includes `time_kind` + `recorded_utc_ms` (L4 §10.2).

### Revision 21 changelog (Architect repair round 20)

- **P0 (this file + L4)**: provenance allowlist authorized `UnknownBootstrap` only for the `RateLimitFreeze` frame, but a new freeze episode must Ack `FreezeEpochWatermark` first — watermark append deadlocked under no published clock. Fixed: closed episode allowlist (watermark → freeze → same-epoch probes while clock unpublished); `FrameTimeKind` canonical in L4 `durable_control_plane.hpp`; `append_freeze_epoch_watermark(..., FrameTimeKind)` (§6.1.1 / L4 §7.3.1 / §10).
- **P1 (cross-file)**: `GenerationSeal` remote Ack did not advance `LastRemoteAckedTip` before `CURRENT` flip — see L4 revision 20.

### Revision 22 changelog (Architect repair round 21)

- **P0 (cross-file)**: L4 "no trustworthy UTC → `source=3`" demoted 418 permanent / long Retry-After — see L4 revision 21 (kind first, provenance second).
- **P0 (this file + L4)**: cleared freeze epochs (`FreezeProbeAttempt.cleared=true`) were still treated as active at recovery and retained forever by compaction rule 2 (`every source==3`). Fixed §6.1.3: retain **active** freezes only; cleared freeze+probe history may be dropped; watermark always retained.
- **P0 (this file)**: §6.1.3 compaction steps still said seal Ack → flip `CURRENT` while L4 required tip in between. Synchronized: seal Ack → durable monotonic `LastRemoteAckedTip` → `CURRENT`; tip failure aborts flip.
- **P0 (cross-file)**: `FreezeEpochWatermark` "epoch it names" vs `next_freeze_epoch` contradiction — see L4 revision 21.
- **P1 (cross-file)**: export-worker tip vs seal tip regression race — see L4 §10.2.1 monotonic CAS / pre-seal drain.

### Revision 23 changelog (Architect repair round 22)

- **P0 (cross-file)**: max-uncleared-epoch recovery hid older `source=2` behind newer timed freezes — see L4 revision 22 (single active episode + merge; aggregate recovery; compaction retain/fold all uncleared).
- **P0 (cross-file)**: overloaded `cleared=true` could lift permanent without operator artifact — see L4 revision 22 (`FreezeClear` + `FreezeClearKind` + `append_freeze_clear`; probe/wait illegal for `source=2`).
- **P0 (this file)**: §6.1.1 provenance + §6.1.3 retain rules updated for `FreezeClear`, multi-uncleared retain/fold, and permanent-not-droppable.

### Revision 24 changelog (Architect repair round 23)

- **P0 (cross-file)**: same-epoch UTC→unknown merge fail-open — see L4 revision 23 (copy-forward `deadline_utc_ms`, max both waits, fold-all-frames).
- **P0 (cross-file)**: `ProbeVerified` without `/time` proof — see L4 revision 23 (`FreezeTimeProbeProof`).
- **P1 (cross-file)**: probe budget reset on non-clearing `/time` success; `utc`/`steady` representability; Windows dir-flush honesty — see L4 revision 23.
- **P0/P1 (this file)**: §6.1.1 / §6.1.3 sync — unknown merge may copy-forward UTC in payload; compaction folds max across all frames of an epoch; fault-inject matrix extended.

### Revision 25 changelog (Architect repair round 24)

- **P0 (this file, self-inflicted, cross-file)**: `is_exchange_final()`'s implementation (§6.1.3) included `Reconciled`, contradicting §6.5's later, authoritative statement that it returns true only for `Filled`/`Cancelled`/`Rejected`/`Expired`. Fixed: `Reconciled` removed — no code path in this design ever targets it as a live transition, so this costs nothing correct while failing closed against a corrupted/legacy `Reconciled` on a still-open order.
- **P0/P1 (cross-file)**: L4 §7.3.1's `FreezeTimeProbeProof` ABI gap, the 8-attempt probe self-lock on long timed freezes, the dual-deadline `ConservativeWaitCompleted` bypass, the unchecked `TimePoint` addition, and the proof's trust-boundary/replay binding — see L4's revision 24 changelog for the full detail; also includes a self-caught function-ordering bug found and fixed during this round's own cross-reference pass.

### Revision 26 changelog (Architect repair round 25)

- **P0 (cross-file)**: dual-deadline `ProbeVerified` could clear before conservative wait finished — see L4 revision 25 (`FreezeWaitSatisfied` + consume-on-clear).
- **P0 (cross-file)**: `FreezeProbeCredit` crash-reset of 8-cap — see L4 revision 25 (`restore_from_recovery`).
- **P1 (cross-file)**: `ClockOffsetSnapshot::seq` wrap + signing/rotation checked arithmetic — see L4 revision 25.
- **P0 (this file)**: §6.1.1 provenance + §6.1.3 retain `FreezeWaitSatisfied` for uncleared dual-deadline epochs; fault-inject matrix extended.

### Revision 27 changelog (Architect repair round 26)

- **P0 (cross-file)**: `FreezeWaitSatisfied` forgeable without elapsed wait — see L4 revision 26 (`FreezeWaitArm` + sink-measured elapsed).
- **P0 (cross-file)**: probe budget bypass via send-before-Ack + kill -9 — see L4 revision 26 (Ack-before-send linearization).
- **P1 (cross-file)**: backoff skip on restore; epoch reset via foreign `try_reserve`; proof MAC host ABI gap — see L4 revision 26.
- **P0 (this file)**: §6.1.1 provenance allowlist + §6.1.3 retain Arm+WaitSatisfied; fault-inject matrix extended to round 26.

### Revision 28 changelog (Architect repair round 27)

- **P0 (cross-file)**: long UTC deadline not durable on probe schedule → crash-loop 8-cap self-lock — see L4 revision 27 (`not_before = max(backoff, folded deadline)` + restore fold + no UTC-gate skip).
- **P1 (cross-file)**: sink ordinal/epoch/not_before enforcement + Ack sequence bind; checked not-before arithmetic — see L4 revision 27.
- **P0 (this file)**: fault-inject matrix extended for deadline-durable probe + sink ordinal reject.

### Revision 29 changelog (Architect repair round 28)

- **P0 (cross-file)**: freeze-period once-per-boot `TimeResyncCredit` restart-launderable `/time` — see L4 revision 28 (mutual exclusion; clock re-publish only via durable `FreezeProbeAttempt` under the 8-cap).
- **P0 (this file)**: provenance + fault-inject matrix for no-freeze-TimeResync bypass.

### Revision 30 changelog (Architect repair round 29)

- **P0 (cross-file)**: `ClockRepublish` last-ordinal self-lock — see L4 revision 29 (`ClockRepublishOrVerify` may `ProbeVerified`-clear; wait-before-probe).
- **P0 (this file)**: fault-inject matrix must-pass for attempts=7→crash→ordinal8→clear.

### Revision 31 changelog (Architect repair round 30)

- **P0 (cross-file)**: `FreezeProbeCredit::try_reserve_probe(..., bool wait_ok)` trusted a caller-supplied boolean with no independent sink-side verification, letting a buggy caller Ack a probe attempt (burning a durable ordinal + licensing a `/time` send) before the conservative wait was actually satisfied — see L4 revision 30 (`append_freeze_probe_attempt()` now independently re-verifies `conservative_wait_ms == 0 || matching sink-verified FreezeWaitSatisfied` before Ack; `wait_ok`/step-1's local check downgraded to fast-path only). No change required in this file — the affected subsystem is L4-only.

### Revision 32 changelog (Architect repair round 31)

- **P0 (cross-file)**: equal-duration WaitSatisfied rematch after a new unknown 429 — see L4 revision 31 (`wait_generation`; Arm/Satisfy/probe/clear/recovery bind current gen).
- **P0 (this file)**: §6.1.1 provenance / §6.1.3 compaction retain Arm+WaitSatisfied **for current `wait_generation` only**; fold `max(wait_generation)`; fault-inject matrix must-pass for equal-wait rematch + stale-gen reject after compaction/recovery.
- **P1 (cross-file)**: per-frame wait = event contribution (not restated max); Failed clear must not "consume" WaitSatisfied — see L4 revision 31.

### Revision 33 changelog (Architect repair round 32)

- **P0 (cross-file)**: legacy upgrade `gen=0→1` + one-shot Satisfy promote re-opened rematch fail-open — see L4 revision 32 (never promote legacy Satisfy; migration/compaction seal to explicit gen `≥1`; sequence-proven rewrite only when Satisfy.seq > last wait-bearing freeze).
- **P0 (this file)**: §6.1.3 compaction of legacy-wait epochs emits aggregate `wait_generation=1`, drops unproven legacy Arm/Satisfy (or sequence-proven rewrite in the same transaction); fault-inject matrix must-pass for `429→Satisfy→429→upgrade`.

### Revision 34 changelog (Architect repair round 33)

- **P0 (cross-file)**: sequence-proven rewrite required only Satisfy after last wait-bearing — Arm-before-rematch could credit pre-rematch elapsed time — see L4 revision 33 (both Arm.seq and Satisfy.seq must be strictly after every wait-bearing freeze).
- **P0 (this file + L4)**: compaction folded snapshot vs live `append_rate_freeze` `prior+1` ABI contradiction — §6.1.3 now mandates `append_compacted_freeze_snapshot` / `append_compacted_wait_evidence` only; never live freeze/wait appends for retain.

### Revision 35 changelog (Architect repair round 34)

- **P0 (cross-file)**: compaction yield allowed N tip to advance (new 429) while stale N+1 still sealed — see L4 revision 34 (`CompactionSourceBaseline` tip re-verify after every yield / before seal; mismatch aborts candidate; `GenerationBridge.prev_tip` must equal archived N final tip).
- **P1 (cross-file)**: generation `UINT32_MAX` exhaustion fence; single-frame `CompactedFreezeWaitEvidencePayload` (no dual-frame optionality).
- **P0 (this file)**: §6.1.3 atomic switch steps include tip-pin checks; fault-inject `freeze/OrderEvent after scan before seal → candidate abort/rebuild → no event loss`.

### Revision 36 changelog (Architect repair round 35)

- **P0 (cross-file)**: seal-quiesce volatile replay buffer lost 429 across power-loss — see L4 revision 35 (append-to-N+abandon default, or durable seal journal; busy-without-durable illegal for received 429/order outcomes).
- **P1 (this file)**: §6.1.3 `GenerationBridge` / seal / recovery now bind `prev_key_id` with `{prev_generation, prev_tip_seq, prev_tip_mac}` (L4 4-tuple); fault-inject seal-window 429 + power-loss.

### Revision 37 changelog (Architect repair round 36)

- **P0 (cross-file)**: path A after `GenerationSeal` export starts / Ack orphaned seal / tip@N+1 vs anti-rollback — see L4 revision 36 (**PreSeal** A|B before entering export; **PostSeal** B-only from export entry; tip drift → `Corrupt`; journal drain flip-then-replay onto N+1).
- **P1 (this file)**: §6.1.3 atomic switch + recovery + fault matrix synchronized to PreSeal/PostSeal; tip equalities clarify `prev_key_id` vs seal `new_key_id`; evidence carries `source_baseline_key_id`.

### Revision 38 changelog (Architect repair round 37)

- **P0 (cross-file)**: remote seal persist + local power-loss before Ack looked PreSeal — see L4 revision 37 (`SealExportStarted` before wire write; `query_seal_by_request_id` recovery).
- **P0 (cross-file)**: PostSeal journal-full deadlock — see L4 revision 37 (network quiesce + drain before started; `kPostSealJournalReserve`; owner journal handoff; no early-flip to free space).
- **P1 (this file)**: §6.1.3 / fault matrix synchronized to `SealExportStarted`, journal metadata, and PostSeal handoff.

### Revision 39 changelog (Architect repair round 38)

- **Note**: GPT re-filed round-37 SealExportStarted / journal-full / `journal_seq` / RAM-callback items — already closed in L4 r37 / this file r38; not reopened.
- **P0 (cross-file)**: `query Unavailable` ≠ “remote never got seal” — see L4 revision 38 (keep `SealExportStarted`, fence, re-query; abandon only on authenticated negative proof).
- **P1 (this file)**: §6.1.3 tip-export drain **before** `SealExportStarted`; direct `export_and_wait_ack`; recovery/fault matrix synced to Unavailable≠absent + seal MAC/content-root bind.

### Revision 40 changelog (Architect repair round 39)

- **P0 (cross-file)**: `SealQueryStatus` replaces misuse of `RecoveryScanStatus` for seal queries — see L4 revision 39 (`Found` / `NotFound` / `TransportUnavailable` / `Corrupt`).
- **P0 (cross-file)**: durable `SealIdWatermark` for non-reusable `request_id`/`candidate_id`; journal exactly-once via `SealJournalApplied` (r39 two-phase; **r40 withdraws two-phase** — see Revision 41); true SPSC / per-producer SPSC handoff — see L4 revision 39.
- **P1 (this file)**: §6.1.3 / fault matrix synchronized to watermark reservation, `SealQueryStatus`, exactly-once journal apply, and single-producer handoff.

### Revision 41 changelog (Architect repair round 40)

- **P0 (cross-file)**: seal-journal exactly-once is **one** `append_seal_journal_apply` frame (origin key + embedded payload); two-phase marker withdrawn — see L4 revision 40.
- **P0 (cross-file)**: PreSeal must **pause tip producer + empty `ExportOutboxRing` + finish tip RPCs** before `SealExportStarted`; remote hard-rejects lagging tips; residual gen-N tuples never re-sent after tip@N+1 — see L4 revision 40.
- **P1 (cross-file)**: complete MAC-valid `SealExportStarted` ⇒ PostSeal (Windows dir-flush undecidable) — see L4 revision 40.
- **P1 (this file)**: §6.1.3 steps 3–5 / recovery / fault matrix synced to atomic journal apply, full outbox drain, and breadcrumb honesty.

### Revision 42 changelog (Architect repair round 41)

- **P0 (cross-file)**: journal de-dup index = `{candidate_id, journal_seq}` only (`entry_mac` comparison) — see L4 revision 41.
- **P0 (cross-file)**: `append_seal_journal_apply(SealJournalAppliedView)` with `std::span` payload — see L4 revision 41.
- **P0 (cross-file)**: durable `SealIdWatermark` advance **before** `SealExportStarted`; recovery requires watermark past Started ids — see L4 revision 41.
- **P1 (this file)**: §6.1.3 step 3 / recovery / fault matrix synced to View ABI, de-dup key, and watermark-then-Started order.

### Revision 43 changelog (Architect repair round 42)

- **P0 (cross-file)**: durable reserve `candidate_id`+`request_id` **before** Path B / in-flight drain; journaled-but-not-Started → replay-to-N only — see L4 revision 42.
- **P0 (cross-file)**: journal admission enforces same size/type/schema as apply (`kSealJournalMaxEmbeddedBytes`) — see L4 revision 42.
- **P1 (cross-file)**: `is_seal_journal_embeddable_type()` closed allowlist; `record_type == embedded_type` — see L4 revision 42.
- **P1 (this file)**: §6.1.3 step 3 / recovery / fault matrix synced to id-before-B, journal↔apply closed loop, and allowlist.

### Revision 44 changelog (Architect repair round 43)

- **P0 (cross-file)**: seal-journal entry + `SealJournalAppliedView` carry durable `time_kind` + `recorded_utc_ms`; apply/replay reuse verbatim; `append_seal_journal_apply` drops separate `FrameTimeKind` arg — see L4 revision 43.
- **P1 (this file)**: §6.1.1 / §6.1.3 / fault matrix synced — journaled frames obey the same provenance rules as live appends; hard-lag age uses preserved `recorded_utc_ms`.

### Revision 45 changelog (Architect repair round 44)

- **P1 (cross-file)**: journal baseline tip 4-tuple authenticated in `entry_mac` + verified on recovery/apply (PostSeal == Started/bridge; PreSeal-abandon intra-candidate only) — see L4 revision 44. Rejects MAC-omitted “dead field” false security.
- **P1 (this file)**: §6.1.3 / fault matrix synced to authenticated baseline bind.

### Revision 46 changelog (Architect repair round 45)

- **P0 (cross-file)**: PreSeal **B-sticky** — Path A illegal after any journal Ack for `candidate_id` (prevents observation-order inversion on replay); unapplied journals + tip≠baseline → `Corrupt` — see L4 revision 45.
- **P1 (cross-file)**: `SealExportStarted` copies exact id-reserve baseline pin; `Found` requires full baseline 4-tuple bind; committed marker == `entry_mac`; `kSealJournalMaxBytes` formula — see L4 revision 45.
- **P1 (this file)**: §6.1.3 / recovery / fault matrix synced — `NotFound` abandon requires tip==baseline (was sibling fail-open vs L4); B-sticky + Found/Started baseline rules.

### Revision 47 changelog (Architect repair round 46)

- **P0 (cross-file)**: every admit / recovery-load / `append_seal_journal_apply` **MUST** recompute `HY-SEALJRN-v1` (opaque `entry_mac` compare withdrawn); old MAC + tampered payload → `Corrupt`, zero business effects — see L4 revision 46.
- **P1 (cross-file)**: packed `SealJournalEntryWire` (FixedMeta=138) + derived MaxBytes; soft-cap = slots **and** byte sum; `kek_key_id` in MAC domain — see L4 revision 46.
- **P1 (this file)**: §6.1.3 / fault matrix synced — withdraw “entry_mac comparison only”; require wire layout + MAC recompute.

### Revision 48 changelog (Architect repair round 47)

- **P0 (cross-file)**: Acked final `.sj1` may not be silently discarded; `SealJournalCommitWatermark` + continuity `1..commit_hw`; damaged/missing middle → `Corrupt` — see L4 revision 47.
- **P1 (cross-file)**: temp→publish→watermark→Ack; framing fields in MAC domain; owner soft-cap counters — see L4 revision 47.
- **P1 (this file)**: §6.1.3 / fault matrix synced — continuity classifier before journal drain/flip; must-pass `Acked 1,2 → delete/truncate 2 → Corrupt`.

### Revision 49 changelog (Architect repair round 48)

- **P0 (cross-file)**: `.sj1` publish MUST be **no-replace**; final exists → MAC+byte-equal idempotent Ack or `Corrupt` (original unchanged) — see L4 revision 48.
- **P1 (cross-file)**: `SealJournalTombstoneWire` + GC after `.jhw` clear; soft-cap = live `.sj1` only; `.jhw` monotonic — see L4 revision 48.
- **P1 (this file)**: §6.1.3 / fault matrix synced — refuse-reserve until full drain (`.jts`/`.jhw` cleared); must-pass replace-overwrite and counter return-to-zero.

### Revision 50 changelog (Architect repair round 49)

- **P0 (cross-file)**: drain-complete end state = no `.sj1`/`.jhw`/`.jts` (r48 required live `.jts` after GC deleted them → refuse-reserve starvation); counter `+=` uses `prev_hw` rule (both counters) — see L4 revision 49.
- **P1 (cross-file)**: partial-GC resume after `.jhw` gone; dual `.jts`+`.sj1` → verify then unlink; orphan `.jts` → `Corrupt` — see L4 revision 49.
- **P1 (this file)**: §6.1.3 / fault matrix synced — drain-complete + `prev_hw` counter must-pass.

### Revision 51 changelog (Architect repair round 50)

- **P0 (cross-file)**: Windows journal publish preferred `CreateHardLinkW`+parent flush; CREATE_NEW copy fallback only; `-=` only after unlink parent flush — see L4 revision 50.
- **P1 (cross-file)**: normative crash-window table `.tmp→…→GC A/B`; preallocated ≤`MaxEntryBytes`; `.jhw` REPLACE vs final no-replace split — see L4 revision 50.
- **P1 (this file)**: §6.1.3 / fault matrix synced — Windows IO + every crash-window cut must-pass.

### Revision 52 changelog (Architect repair round 51)

- **P0 (cross-file)**: intake-closed barrier before GC A; `.jhw` covers Path B lifetime; `Started`+missing `.jhw` → Path B fail-closed (never invent seq) — see L4 revision 51.
- **P1 (this file)**: §6.1.3 / fault matrix synced — concurrent PostSeal handoff at GC A must-pass.

### Revision 53 changelog (Architect repair round 52)

- **P0 (cross-file)**: clear `SealExportStarted` only at drain-complete (full flip→apply→tombstone→intake-close→GC chain); crash-window early-clear withdrawn — see L4 revision 52.
- **P0 (cross-file)**: linearizable `SealJournalIntakeCloseControl` (close epoch / admit guard / quiesced ACK / double-confirm drain) — see L4 revision 52.
- **P1 (this file)**: §6.1.3 / fault matrix synced — Tip@N+1 early-clear + late SPSC tail must-pass.

### Revision 54 changelog (Architect repair round 53)

- **P0 (cross-file)**: freeze `registered_producer_mask`/`producer_count`/`ring_id[]` before PostSeal; close waits mask-only — see L4 revision 53.
- **P1 (cross-file)**: `close_deadline_steady` + max poll; timeout → retain `.jhw` + fence + no GC/clear Started — see L4 revision 53.
- **P1 (this file)**: §6.1.3 / fault matrix synced — true-SPSC mask=1 + hung-producer deadline must-pass.

### Revision 55 changelog (Architect repair round 54)

- **P0 (cross-file)**: post-close apply catch-up before GC A; durable topology MAC-bound into `SealExportStarted` — see L4 revision 54.
- **P1 (cross-file)**: unset-bit admit → immediate fence; timeout retry bumps `close_epoch`; `static_assert(kMax≤8)` — see L4 revision 54.
- **P1 (this file)**: §6.1.3 / fault matrix synced — catch-up + Started-topology must-pass.

### Revision 56 changelog (Architect repair round 55)

- **P0 (cross-file)**: L4 §10.3 no longer defines an abbreviated PreSeal/cleanup — delegates to §10.1/§10.2 full chain — see L4 revision 55.
- **P1 (cross-file)**: packed `SealExportStartedWire` v2 (234B) + legacy 192B fail-closed/offline migrate — see L4 revision 55.
- **P1 (this file)**: §6.1.3 / fault matrix synced — single flip-path + StartedWire v2 must-pass.

### Revision 57 changelog (Architect repair round 56)

- **P0 (cross-file)**: Started v1→v2 migrate via companion `.v2` + `.mig` (never replace/delete legacy) — see L4 revision 56.
- **P1 (this file)**: §6.1.3 / fault matrix synced — migrate power-cut cuts never PreSeal / never new ids.

### Revision 58 changelog (Architect repair round 57)

- **P0 (cross-file)**: `kek_key_id` on Started v2 (238B) + mig v2; `.clr` ordered Started clear — see L4 revision 57.
- **P1 (cross-file)**: closed L↔V equalities + full-file digests in M — see L4 revision 57.
- **P1 (this file)**: §6.1.3 / fault matrix synced — no try-all KEK; `.clr` mid-clear resumes.

### Revision 59 changelog (Architect repair round 58)

- **P0 (cross-file)**: `.clr` CREATE/CleanupInProgress bound to PostSealCommittedProof (CURRENT flip + LastRemoteAckedTip + bridge + journal drain) — see L4 revision 58.
- **P1 (this file)**: §6.1.3 / fault matrix synced — ClrUnauthorized → query-first; Started+zero-journal+wrongful `.clr`+Found must-pass.

### Revision 60 changelog (Architect repair round 59)

- **P1 (cross-file)**: ClrAbandoned (`.abd`) converges ClrUnauthorized + NotFound — see L4 revision 59.
- **P1 (this file)**: §6.1.3 / fault matrix synced — NotFound abandon must clear unauthorized C via `.abd`; ids never reused.

### Revision 61 changelog (Architect repair round 60)

- **P1 (this file)**: §6.1.3 (g) greenfield Started write refuses residual `.abd` (aligned with L4 Enter PostSeal); must-pass: durable `.abd` + watermark reserved → refuse Started until ResumeAuthorized + unlink A (r61 said “A3”; r63 clarifies).

### Revision 62 changelog (Architect repair round 61)

- **P1 (cross-file)**: ClrAbandoned GenGone + TipExportProducerResume — see L4 revision 61.
- **P1 (this file)**: §6.1.3 compaction/recovery sync — NotFound/`.abd` abandon resumes tip-export producer and abandons `gen-N+1/`.

### Revision 63 changelog (Architect repair round 62)

- **P1 (cross-file)**: ResumeAuthorized before unlink `.abd`; GenGone no-replace + Corrupt if G+T missing — see L4 revision 62.
- **P1 (this file)**: §6.1.3 (g)/refuse — new Started only after ResumeAuthorized + unlink `.abd` + TipExportProducerResume (producer stays paused through ResumeAuthorized; never resume-before-unlink).

### Revision 64 changelog (Architect repair round 63)

- **P0 (cross-file)**: authenticated-NotFound abandon without C must still A0 `.abd` before clearing Started — see L4 revision 63.
- **P0 (this file)**: §6.1.3 (g)/refuse/recovery/fault matrix — unify with-C and no-C onto A0→…→ResumeAuthorized→unlink A→TipExportProducerResume; no-Started+no-A+(G|T) → Corrupt/IoError (not PreSeal) — **narrowed by revision 65** via `CompactionCandidateIntent`; Path-A PreSeal-abandon still does not require `.abd`; TransportUnavailable never writes `.abd`.

### Revision 65 changelog (Architect repair round 64)

- **P0 (cross-file)**: r63/r64 `no Started + no A + (G|T) → Corrupt` false-positive on legal PreSeal-build crash (G fsynced before reserve/Started) — see L4 revision 64 (`CompactionCandidateIntent` Building→Reserved→StartedPublished).
- **P0 (this file)**: §6.1.3 write order / recovery / fault matrix — Intent CREATE before `gen-N+1/`; Building|Reserved+no Started+no A → PreSeal-abandon (not Corrupt); StartedPublished+no Started+no A → Corrupt/IoError (**narrowed by revision 66** — terminal Finalizing phases are NOT Corrupt); Intent clear only after final cleanup; must-pass `G fsync→crash→cleanup+retry` and `StartedPublished→Started/A missing→Corrupt`.

### Revision 66 changelog (Architect repair round 65)

- **P0 (cross-file)**: PostSeal / ClrAbandoned completion crash window false-Corrupt via StartedPublished Intent — see L4 revision 65 (terminal Intent phases **PostSealFinalizing** / **AbandonFinalizing** on same 140B wire).
- **P0 (this file)**: §6.1.3 cleanup order / recovery / fault matrix — PostSeal: `.clr` Authorized → Intent PostSealFinalizing **before** Started unlink → ordered unlink → TipExportProducerResume → clear Intent; Abandon: ResumeAuthorized → Intent AbandonFinalizing **while A present** → unlink A → resume → clear Intent; recovery from either Finalizing finishes (NOT Corrupt); Corrupt only for non-terminal StartedPublished+no Started+no A; must-pass power-cut after last `.clr`/Started gone before Intent clear, and after `.abd` unlink before Intent clear.

### Revision 67 changelog (Architect repair round 66)

- **P1 (cross-file)**: Intent phase-cross / no-skip not crash-verifiable from REPLACE-only `phase` — see L4 revision 66 (`CompactionIntentTransitionWire` 176B; `HY-COMPINTENT-X-v1`; no-replace `.x1` before every Intent phase REPLACE; recovery chain verify + one-step lag; GC `.x1` before Intent unlink).
- **P1 (this file)**: §6.1.3 raise order / recovery / fault matrix — every Intent phase raise persists matching `.x1` **before** REPLACE; genesis = CREATE Building (no none→Building `.x1`); recovery verifies legal chain or one-step lag (complete REPLACE); illegal PostSeal↔Abandon / jump + crash → Corrupt (not clear Intent); power-cut after `.x1` before Intent REPLACE → converge; PreSeal-abandon and final clear GC transitions with Intent; orphan `.x1` → Corrupt; narrowed claim: Intent phase monotonic REPLACE alone does **not** prove no-cross.


### Revision 68 changelog (Architect repair round 67)

- **P0 (cross-file)**: multi-file `.x1` GC power-window false-Corrupt — see L4 revision 67 (`CompactionIntentGcAuthorizedWire` 204B; `HY-COMPINTENT-GC-v1`; no-replace `.xgc` before any `.x1` unlink; GC order `.xgc` → `.x1`* → Intent → `.xgc` last; Mode B recovery converges authorized gaps; r66 GC-lag without auth withdrawn).
- **P0 (this file)**: §6.1.3 cleanup / recovery / fault matrix — terminal clear CREATE `.xgc` then GC `.x1` then Intent then unlink `.xgc`; recovery Mode B under verified `.xgc` (partial `.x1` / Intent-gone+`.xgc` converge, not Corrupt); no `.xgc` + gap while Intent live → Corrupt; forged/dual/early `.xgc` → Corrupt; must-pass power-cut before/after each seq unlink / Intent unlink / `.xgc` unlink.

### Revision 69 changelog (Architect residual sync round 68)

- **P0 (cross-file residual)**: r67 `.xgc` ABI/Mode A·B were correct, but L4 Recovery/crash-window/must-pass still had TipExportProducerResume → clear I / bare “seq gap → Corrupt” paths that could reintroduce the multi-file GC false-Corrupt — see L4 revision 68 (every terminal clear path CREATE `.xgc` → GC `.x1`* → Intent → `.xgc` last; Mode-A-only gap Corrupt; Building-only zero `terminal_transition_mac` carve-out).
- **P0 (this file residual)**: §6.1.3 must-pass / recovery abbreviations that still said resume → clear I (or finish+clear I) without `.xgc` — aligned to L4 r68; ClrAbandoned / PostSealFinalizing / PreSeal-abandon / AbandonFinalizing finish paths all require `.xgc`-authorized clear; must-pass zeros-mac traps for Reserved vs Building-only.

### Revision 70 changelog (Architect residual sync round 69)

- **P0 (cross-file)**: L4 §10.3 Cross-consistency still abbreviated every `CURRENT` flip as `… → Intent PostSealFinalizing → ordered unlink → clear Intent` (`.xgc` tail omitted) — see L4 revision 69 (full chain + §10.3 flip crash-window Mode B rows + Forbidden against abbreviated terminal paths).
- **P0 (this file residual)**: §6.1.3 must-pass / fault-matrix abandon tails that still ended `…→unlink A→resume` or `PostSeal…→ordered unlink;` without TipExportProducerResume + CREATE `.xgc` → GC `.x1` → clear Intent → unlink `.xgc` — aligned to L4 r69; every live normative PostSeal/Abandon terminal clear path requires the full `.xgc` segment.

### Revision 71 changelog (Architect repair round 70)

- **P0 (cross-file)**: early / forged MAC-legal `.xgc` made Mode B undecidable — Mode B skipped re-checking CREATE-time physical cleanup preconditions — see L4 revision 70 (`PhysicalCleanupPreconditions(terminal_disposition)` before Mode B converge; early `.xgc` + leftover Started/C/A → Corrupt; `intent_phase_at_auth=PostSealFinalizing` is not cleanup-complete proof).
- **P0 (this file)**: §6.1.3 recovery / must-pass / fault matrix — withdraw “valid `.xgc` ⇒ Mode B” shorthand; Mode B requires MAC-valid `.xgc` **and** PhysicalCleanupPreconditions; trap rows: Intent-gone + `.xgc` + leftover Started → Corrupt (not finish `.xgc` only); `.xgc` + leftover C only → Corrupt; Abandon `.xgc` + leftover A → Corrupt; dual disposition mismatch → Corrupt; legitimate mid-GC after gates cleared still converges.



### Revision 72 changelog (Architect repair round 71)

- **P0 (cross-file)**: r70 Mode B PhysicalCleanupPreconditions non-constructible after legal mid-GC crash (needed live `.clr`/`.abd` + TipExport “already executed”, but C/A already unlinked before CREATE `.xgc`) — see L4 revision 71 (`CompactionIntentGcAuthorizedWire` **316B** / `format_version=2` / `HY-COMPINTENT-GC-v2` embeds **DurableCleanupAuthEvidence**; capture-while-C/A-readable → unlink C/A → CREATE `.xgc`; Mode B verifies from `.xgc` + live gate absence; TipExportProducerResume idempotent, not a durable recovery precondition; legacy v1 204B fail-closed).
- **P0 (this file)**: §6.1.3 / fault matrix — Mode B after `C deleted → .xgc written → delete any x1 → crash` and `A deleted → .xgc written → delete any x1 → crash` must converge without live C/A; withdraw any shorthand that re-reads `.clr`/`.abd` or treats TipExportProducerResume as Mode B evidence; early `.xgc` + leftover Started/C/A still Corrupt; forged cleanup evidence Corrupt; update all live-normative `.xgc` size/domain references 204/`v1` → **316/`v2`**.


### Revision 73 changelog (Architect residual sync round 72)

- **P0 (cross-file residual)**: L4 r71 ABI/recovery already required `.xgc` **316B / `HY-COMPINTENT-GC-v2` / DurableCleanupAuthEvidence** with legacy v1/204B fail-closed, but live L4 §10.1 Clear (r67) prose + TipExport/§10.3/Recovery/crash-window Abandon tails + this file’s fault-matrix “GC auth wire **204B**” / `unlink A → resume → CREATE .xgc` still prescribed the old write size or wrong resume-before-CREATE order → deterministic recovery self-lock. Aligned to L4 revision 72: every live normative PostSeal/Abandon clear = **CAPTURE → unlink C/A → CREATE v2 `.xgc` (316B) → TipExportProducerResume (idempotent) → GC `.x1` → clear Intent → unlink `.xgc`**; GC auth wire **316B**; v1/204B only as legacy fail-closed.

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
// CHANGED — fixes round-6 P1: L4 §8 already claimed "separate budgets for
// DNS resolve, TCP connect, TLS handshake, write, and read," but this
// struct — the only config type actually feeding a real implementation —
// only ever had two fields (connect_timeout_ms, read_timeout_ms). "Per-phase
// deadlines" was not implementable from what this spec actually specified.
struct PrivateRestConfig {
    EnvironmentBinding environment;   // from L4 §1 — immutable after init
    std::uint32_t resolve_timeout_ms = 2000;   // NEW — DNS resolution
    std::uint32_t connect_timeout_ms = 3000;   // TCP connect only (was
                                                 // ambiguously "connect",
                                                 // now scoped to just this phase)
    std::uint32_t handshake_timeout_ms = 3000; // NEW — TLS handshake only
    std::uint32_t write_timeout_ms = 2000;     // NEW — writing the request
    // Round-11 P1: Binance matching-engine processing can itself approach
    // ~10s before returning -1007; a 10s *read* deadline leaves ~0 network
    // margin and converts engine-slow-but-alive responses into Timeout →
    // Ambiguous storms under load. Default is matching-engine budget
    // (10s) + network margin (5s) = 15s. Lowering below
    // kMinReadTimeoutMs (= 12000) at config load is rejected (fail closed).
    std::uint32_t read_timeout_ms = 15000;
    // Each phase gets its OWN deadline, applied via Beast's per-operation
    // expires_after()/expires_at() on the underlying beast::tcp_stream —
    // not one shared timer covering multiple phases. A slow DNS resolver
    // consuming the resolve budget cannot eat into the connect budget, etc.
    // (the exact gap L4 §8's original text named but this struct couldn't
    // yet back up).
};
```

**Cancellation and post-timeout connection handling — fixes round-6 P1 (the deadlines above are meaningless without a defined reaction to hitting one)**:

- On any phase's deadline expiring, the in-flight operation is cancelled (`beast::tcp_stream::cancel()`/`expires_after()`'s own timeout-triggers-cancellation behavior) and the call returns the corresponding outcome (`Timeout` for `submit_order()`/`ReconcileQueryOutcome::Inconclusive` for `query_order()`) — never silently waits past the phase's own budget hoping a later phase absorbs the delay.
- **The underlying socket/stream is never reused after any timeout, for any phase.** A connection that timed out mid-handshake or mid-write is in an unknown protocol state — Beast/OpenSSL provide no guarantee the peer agrees where the exchange left off. The stream is torn down (abortive close is acceptable; a clean TLS shutdown is attempted best-effort but not waited on past a short bound, since a hung shutdown is exactly the kind of thing that got the connection into this state) and a fresh connection is established for the next attempt — no connection pooling/keep-alive reuse of a stream that has ever hit a phase timeout.
```cpp

class BinancePrivateRestClient {
public:
    bool init(std::string_view api_key, BinanceSigner& signer,
              const EnvironmentBinding& environment) noexcept;

    // Current registry version (L4 §5.3) — used ONLY for Gate 1's cheap,
    // fast pre-check (SubmitPort::CurrentRulesVersionFn, §2.1), comparing
    // against ctx.pre_trade_rules_snapshot.rules_version. This is
    // deliberately NOT what wire formatting uses (see submit_order() below,
    // fixes round-5 P0 TOCTOU) — a version number alone can't bind what gets
    // formatted, only reject early when it's already obviously stale.
    std::uint32_t current_rules_version() const noexcept;

    // Blocking. Called only from the L5 runtime thread that owns SubmitPort,
    // never from the hot path. Signature matches SubmitPort::SubmitFn (§2.1
    // below) parameter-for-parameter — this is what the function pointer
    // ultimately calls into, so the two signatures must never drift apart.
    SubmitResponse submit_order(
        std::string_view client_order_id,
        std::uint32_t symbol_id, OrderSide side, OrderType type,
        std::int64_t price_ticks, std::int64_t qty_ticks,
        const SymbolRules& rules_snapshot) noexcept;  // CHANGED (fixes round-5
                                                        // P0) — was a bare
                                                        // expected_rules_version
                                                        // integer; this is now
                                                        // the ACTUAL captured
                                                        // SymbolRules value
                                                        // pre-trade validation
                                                        // used (L4 §5.3). Wire
                                                        // formatting (§5.2) uses
                                                        // rules_snapshot.symbol /
                                                        // .price_scale / .qty_scale
                                                        // DIRECTLY — this function
                                                        // performs NO registry
                                                        // lookup of its own, so
                                                        // there is no code path
                                                        // here that could see
                                                        // data newer than what
                                                        // Gate 1 already checked.

    // Implements L4 §6 — called by the orchestrator's Ambiguous-handling path,
    // not by this class internally. Exposed here because it shares the same
    // signing/transport plumbing as submit_order(). Returns Found or
    // Inconclusive per L4 §6.1 — never a "confirmed absent" result.
    ReconcileQueryResult query_order(
        const OrderExpectation& expected) noexcept;  // CHANGED — round 6 was
                                                        // (symbol, orig_client_order_id),
                                                        // round 6.1 was (symbol, expected).
                                                        // Round 7 P1: dropped the separate
                                                        // `symbol` parameter — it could
                                                        // drift from
                                                        // expected.rules_snapshot_at_submit.symbol,
                                                        // two representations of "which
                                                        // symbol" a caller could pass
                                                        // inconsistently. The query
                                                        // string's symbol= parameter
                                                        // (L4 §6) is built from
                                                        // expected.rules_snapshot_at_submit.symbol
                                                        // exclusively now. L4 §6.1.2's
                                                        // schema + field-match validation
                                                        // needs the full expected
                                                        // side/type/price/qty/rules-scale
                                                        // to check a response against,
                                                        // not just an ID to match.

private:
    // Boost.Beast SSL stream. Security contract per L4 §8 — hostname
    // verification, per-phase deadlines, response-size cap are tested
    // properties of THIS class, not inherited from binance_rest_snapshot.hpp.
};
```

### 2.1 `SubmitPort` ABI change — fixes round-3 P0 (`rules_version` stopped one layer short of where it needed to go), updated for round-5's TOCTOU fix

Round 3's fixes threaded a `rules_version` integer into `submit_order()` (§2 above), but that function is only ever *called from* `SubmitPort` — the actual injection-point ABI in `live_submit_orchestrator.hpp` — and that ABI was never updated to carry it. Round 5 additionally found (L4 §5.3) that a bare integer isn't enough even once threaded through: it lets Gate 1 fast-reject an obviously-stale order, but says nothing about what data wire-formatting itself uses later. `SubmitFn`/`call()` therefore carry the **captured `SymbolRules` snapshot**, not just its version number — matching `submit_order()`'s §2 signature change:

```cpp
// live_submit_orchestrator.hpp — SubmitPort gains a SECOND function pointer
// (§2.2 explains why this can't just be a parameter on SubmitFn/call()):
struct SubmitPort {
    using SubmitFn = SubmitResponse(*)(
        const char* client_order_id,
        std::uint32_t symbol_id,
        OrderSide side,
        OrderType type,
        std::int64_t price_ticks,
        std::int64_t qty_ticks,
        const SymbolRules& rules_snapshot,  // CHANGED (round-5 P0) — was a bare
                                             // expected_rules_version integer;
                                             // now the actual captured snapshot,
                                             // matching submit_order()'s §2 change
        void* user_data);

    // NEW — a lightweight, side-effect-free query, deliberately separate from
    // SubmitFn. See §2.2: this must be callable BEFORE any audit write,
    // CONFIRM check, or in-flight registration, so it cannot be folded into
    // the same call as the actual submit. Used ONLY for Gate 1's cheap
    // fast-reject pre-check — never for wire formatting (that uses the
    // snapshot above, not a fresh lookup — see §2.2).
    using CurrentRulesVersionFn = std::uint32_t(*)(void* user_data);

    SubmitFn fn{nullptr};
    CurrentRulesVersionFn current_rules_version_fn{nullptr};
    void* user_data{nullptr};

    SubmitResponse call(const char* coid, std::uint32_t sym,
                        OrderSide side, OrderType type,
                        std::int64_t price, std::int64_t qty,
                        const SymbolRules& rules_snapshot) const noexcept {
        if (!fn) return {SubmitOutcome::NetworkError, 0, -1};
        return fn(coid, sym, side, type, price, qty, rules_snapshot, user_data);
    }

    // 0 is reserved as "unknown/unavailable" and never matches a real
    // rules_version (L4 §5.1's registry versions start at 1) — a missing
    // function pointer therefore fails closed by construction, not by an
    // extra null check the caller has to remember.
    std::uint32_t current_rules_version() const noexcept {
        if (!current_rules_version_fn) return 0;
        return current_rules_version_fn(user_data);
    }

    bool is_valid() const noexcept { return fn != nullptr; }
};
```

### 2.2 Why the version check must be its own gate, before `Submitting`, AND why it alone isn't sufficient — fixes round-4 P0 and round-5 P0

Revision 4 described the version comparison as happening *inside* `submit_order()` — but `submit_order()` is only ever reached via `SubmitPort::call()`, which §3's gate list places *after* the durable audit-intent write, CONFIRM/port-validity checks, in-flight registration, and the `Submitting` transition. The comparison living inside `submit_order()` therefore couldn't be "before `Submitting`" — the exact same class of self-contradiction round 3 found in `RateLimited`. Fixed in revision 5: the version check is its own gate, first in §3's list, run before any state mutation.

Round 5 found a second, deeper problem with that gate even after it's correctly positioned: comparing `current_rules_version()` (an integer, freshly read at Gate 1's moment) against `ctx.pre_trade_rules_snapshot.rules_version` only proves the versions matched *at Gate 1's instant*. Gates 2–8 (policy, audit writes, CONFIRM, in-flight registration, joint reservation) all take real time; if `submit_order()`'s wire-formatting step (Gate 9) went back to the registry for `symbol`/`price_scale`/`qty_scale` at ITS OWN moment, a refresh landing anywhere in that window would format the request from data no gate ever actually checked — narrowing the window doesn't remove this, only shrinks how often it's hit.

**Fix (L4 §5.3): `submit_order()` never looks the registry up a second time.** Gate 1 remains a cheap fast-reject using the version integer (below), but the thing that makes the design correct is that `ctx.pre_trade_rules_snapshot` — the actual `SymbolRules` copy taken once, at pre-trade-validation time — is what travels all the way to `submit_order()`/`SubmitFn` and is the ONLY source wire-formatting consults:

```cpp
// Gate 1, first in §3's list, strictly before the audit-intent write.
// Cheap fast-reject only — NOT what makes the design correct (see above);
// it just avoids paying for gates 2-7 when staleness is already obvious.
if (ctx.submit_port.current_rules_version() != ctx.pre_trade_rules_snapshot.rules_version) {
    result.gate = OrchestratorGate::SubmitStaleRulesVersion;
    return result;  // fail-closed — no audit write, no Submitting, no in-flight
                     // registration have happened yet at this point
}

// ... gates 2-7 ...

// After Gate 7 (joint reservation, §3) succeeds, Gate 8 (sign/format) passes
// ctx.pre_trade_rules_snapshot itself, not a re-derived lookup:
auto response = ctx.submit_port.call(coid, ctx.symbol_id, ctx.side, ctx.order_type,
                                      ctx.price_ticks, ctx.qty_ticks,
                                      ctx.pre_trade_rules_snapshot);
```

`submit_order()` (§2) has no registry member to consult at all for formatting — it receives every field it needs (`symbol`, `price_scale`, `qty_scale`) as part of `rules_snapshot`. There is structurally no code path inside it that could see data newer than what Gate 1 already fast-checked, which is what actually closes the TOCTOU (Gate 1 alone only ever narrows it).

## 3. Gates checked before every send

Ordering matches §6.2's actual code shape — the durable audit check is two separate gates (intent, then conservative prepared boundary), not one. §2.2's version check is now genuinely first, resolving round-4's finding; round 5's fix means the gate's own comparison is a fast-reject only, not the source of correctness (§2.2):

1. **Symbol registry version fast-reject (L4 §5.3)** — `ctx.submit_port.current_rules_version()` vs `ctx.pre_trade_rules_snapshot.rules_version`; fail closed (`StaleRulesVersion`) on mismatch. Pure local comparison, no network, no audit write, no state mutation yet. The actual TOCTOU-closing mechanism is `ctx.pre_trade_rules_snapshot` traveling unchanged to Gate 8's sign/format (**fixed this round** — was mis-numbered "Gate 9" here, inconsistent with the numbered list below and with line ~415's correct "Gate 8 (sign/format)"), not this comparison by itself.
2. `transport_policy::validate_policy()` — once at startup, against the bound environment's policy (L4 §1).
3. `transport_policy::check_endpoint()` — resolved host must be in *this environment's* allowlist.
4. Server-time offset freshness (L4 §2.2) — fail closed if TTL-expired and refresh failed.
5. **`DurableAuditSink::append_durable()` of `OrderIntentCreated` returns `Acked`** (§6.2) — fail closed (`AuditWriteNotAcked`) otherwise, before CONFIRM/port-validity checks even run.
6. Operator CONFIRM + submit-port-validity + in-flight registration in `InFlightRegistry` — **`OrderRecord::state` is NOT transitioned to `Submitting` here** (fixes this round's P0 — an earlier revision paired the `Submitting` transition with this gate, which left gates 7–8's failures with no valid transition out of `Submitting` and motivated the now-removed `AbortedPreSend` state; see §4.3's fix). `InFlightRegistry` registration (idempotency-key reservation) is independent of `OrderRecord::state` and is exactly what gates 7–8's failure paths release.
7. **Joint budget reservation (L4 §7.2 `try_reserve_all_budgets()`)** — weight + order-count + RAW_REQUESTS reserve successfully before any submitted/prepared fact exists. On refusal, release the `InFlightRegistry` registration and leave only the earlier Intent audit record; `OrderRecord::state` is still `Intent` at this point, so nothing needs to be transitioned back. No reconciliation is scheduled.
8. Construct, size-check and sign the exact bytes. A formatting/signing failure is also provably pre-send: release the `InFlightRegistry` registration, append only a best-effort diagnostic while unfenced, and leave the durable history as Intent-only — `OrderRecord::state` is still `Intent`, exactly as after gate 7's failure path. It must never create `OrderSubmitPrepared`.
9. **`DurableAuditSink::append_durable()` of `OrderSubmitPrepared` returns `Acked`** (§6.2) — this means the request is authorized to begin transport, not that a write completed. **`OrderRecord::transition_to(Submitting)` is called here, immediately after this ACK** — the one and only place this transition happens, matching the semantics "authorized to begin transport" this gate already defines. If the append returns `Failed`, it may nevertheless have reached media; fence/stop and retain the COID **without transitioning state** — `OrderRecord::state` remains `Intent`, since the ACK that would have triggered the transition never confirmed. Recovery classifies a complete (physically-present, checksummed) prepared frame found in the durable log as `Ambiguous` regardless of what any in-memory state claimed at crash time.
10. Send via Beast, honoring per-phase deadlines. The single durable Prepared fact is deliberately conservative across every crash point after its ACK: recovery reconciles rather than assuming no write occurred.
11. **Parse rate-limit headers FIRST, from the raw response, for every HTTP status** (**reordered — fixes round-10 P1**: the previous order ran full-schema validation before header/status handling, but a 429/418 body never satisfies §4.2's FULL success schema — freeze persistence sat behind a schema check guaranteed to fail first). Feed `X-MBX-USED-WEIGHT-*` / `X-MBX-ORDER-COUNT-*` into the `BucketIdentity` correction ABI; on 429/418: `parse_retry_after(http_status, ...)` (L4 §7.3) → durably ACK a `RateLimitFreeze` frame (L4 §7.3.1) → `freeze_until()`/`freeze_all_until()` → classify `RateLimited` per §4.4 (no body parse attempted — its absence/shape is irrelevant to the outcome).
12. `transport_policy::check_http_status()` — reject 3xx (no auto-follow); non-2xx statuses classify via §4.4's table (body parsed only for the error `code` field, under the same size guard as Gate 13).
13. `transport_policy::check_response_size()` — cap response body size (the parser-level `body_limit()`/`header_limit()` from L4 §8 already bounded the read itself; this is the defense-in-depth check).
14. For 2xx only: parse response JSON via simdjson; run §4.2's full schema + field-match validation **before** any status-field-based branching (§4.3).

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
    StaleRulesVersion = 5, // NEW — L4 §5.3's fail-closed check. Per §2.2's fix,
                            // this is decided by the orchestrator's own
                            // pre-flight gate directly comparing
                            // ctx.submit_port.current_rules_version() against
                            // ctx.pre_trade_rules_snapshot.rules_version — BEFORE
                            // SubmitPort::call() is ever invoked. This enum
                            // value is therefore never actually returned by a
                            // SubmitFn implementation under the gate ordering
                            // in §3; it exists so OrchestratorGate::
                            // SubmitStaleRulesVersion (below) has a matching
                            // SubmitOutcome for ABI/logging symmetry with
                            // every other gate-to-outcome pairing in this
                            // spec, not because call() can produce it.
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
    bool retry_after_present{false};          // CHANGED (fixes round-7/11 P0)
    std::int64_t retry_after_deadline_ms{0};  // ABSOLUTE UTC wall-clock ms from
                                                // RetryAfterResult::deadline_utc_ms
                                                // (L4 §7.3) — NEVER steady-clock.
                                                // Valid only if retry_after_present;
                                                // populated when outcome == RateLimited.
                                                // In-process freeze_until uses the
                                                // companion deadline_steady from the
                                                // same parse; scheduling converts UTC→
                                                // steady at enqueue (L4 §6.1 / §7.3).
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

1. Parse the full response schema — required fields: `symbol`, `orderId`, `clientOrderId`, `transactTime`, `price`, `origQty`, `executedQty`, `cummulativeQuoteQty`, `status`, `timeInForce`, `type`, `side`. **Fixes round-5 P1**: revision 4's field list omitted `origQty`, `cummulativeQuoteQty`, `timeInForce`, and `type` — none of which were used for the cross-check in step 2 or the fill-data source in step 4, both real gaps, not just a documentation omission. Missing any required field → treat as schema-invalid (§4.4's 200-with-schema-mismatch row).
2. Cross-check against the original request, all of which must match exactly: `symbol`, `clientOrderId`, `side`, `type` (always `LIMIT` — a response reporting anything else is schema-invalid, not a value this spec ever sends), `timeInForce` (always `GTC` — same reasoning), `price` (compared as the same scaled-integer representation used to construct the request, not a floating-point string comparison), `origQty` (**new in round 5** — the previous cross-check list omitted the requested quantity itself, meaning a response reporting a different `origQty` than what was actually sent would have passed validation).
3. Only after 1–2 pass does `status` (`NEW` / `PARTIALLY_FILLED` / `FILLED` / others) select the branch in §4.3's routing table.
4. **Fill data comes from `executedQty`/`cummulativeQuoteQty`, never from `fills[]` directly** (fixes round-5 P1 — round 3 already established that `fills[]` *presence* must not drive the branch, but revision 4 never actually specified where `filled_qty_ticks`/`avg_fill_price_ticks` come from once `status` selects a fill branch). `filled_qty_ticks = executedQty` (parsed at `qty_scale`); `avg_fill_price_ticks` is derived from `cummulativeQuoteQty / executedQty` via the exact same `rescale_notional_ceil`-adjacent integer-division approach L4 §6.1.1 specifies for reconciliation fills — the two code paths (direct POST response vs. `GET /order` reconciliation) must compute a fill's average price identically, since both ultimately populate the same `OrderRecord::avg_fill_price_ticks` field. `fills[]`, when present, is used **only** as an optional cross-check — summing `fills[].qty` and comparing to `executedQty` (both already-parsed integer ticks, integer comparison, no tolerance) — and a mismatch is logged as a schema anomaly but does **not** by itself invalidate the response, since `executedQty`/`cummulativeQuoteQty` remain the authoritative source Binance's own documentation describes them as.

### 4.3 Result routing (was: implicit "orchestrator transitions to Accepted"; now: explicit per-outcome routing)

| `SubmitOutcome` | `OrchestratorGate` | `OrderRecord` transition | `InFlightRegistry` |
|---|---|---|---|
| `Accepted`, `exchange_status == NEW` | `SubmitAccepted` | `Submitting → Accepted` (unchanged) | stays registered (order live on exchange) |
| `Accepted`, `exchange_status == PARTIALLY_FILLED` | `SubmitPartialFill` (**new**) | **`Submitting → PartialFill` directly** (fixes round-9 P0 — see note below), fill fields from `SubmitResponse` | stays registered |
| `Accepted`, `exchange_status == FILLED` | `SubmitFilled` (**new**) | `Submitting → Filled` (already permitted by `order_lifecycle.hpp`) | released (terminal) |
| `Rejected` | `SubmitRejected` | `Submitting → Rejected` (unchanged) | released (terminal) |
| `Timeout` / `NetworkError` | `SubmitAmbiguous` / `SubmitNetworkError` | `Submitting → Ambiguous` (unchanged) | stays registered (unresolved) |
| `RateLimited` (**new**, fixed round-3) | `SubmitRateLimited` (**new**) | `Submitting → Ambiguous` — **identical treatment to Timeout/NetworkError.** By the time this response exists, `Submitting`/in-flight registration already happened; nothing distinguishes a 429/418 from any other post-send uncertainty except that its cause is known (rate limiting) | **stays registered** — reconciled via L4 §6 exactly like any other `Ambiguous` order; additionally freezes all slots in `RequestWeightTrackerSet`/`RawRequestsTrackerSet`/`OrderCountTrackerSet` per L4 §7.3 so the *next* attempt waits appropriately |
| `StaleRulesVersion` (**new**, local-only, genuinely pre-send) | `SubmitStaleRulesVersion` (**new**) | No transition — caught at Gate 1 (§2.2/§3), before `Submitting`, before in-flight registration, before `SubmitPort::call()` is ever invoked | released — this is the one outcome in this table where "never touched the network" is actually true |

Reconciliation `Found` results (L4 §6.5) route into this same table via their mapped confirmed state — `Accepted`/`PartialFill`/`Filled`/`Rejected`/`Cancelled`/`Expired` — never through a separate "Reconciled" terminal that would hide a still-open order.

**PARTIALLY_FILLED / single outcome record — fixes round-9 P0:** an earlier draft wrote `Submitting → Accepted → PartialFill` while defining only one post-response durable outcome frame. Real `order_lifecycle.hpp::validate_transition` allows `Submitting → Filled` but **not** `Submitting → PartialFill` and **not** two transitions from one frame. Writing only `PartialFill` made recovery `Corrupt`; writing only `Accepted` dropped fill data. **Fix (proposed change to `order_lifecycle.hpp`, alongside L4 §6.5's Ambiguous extension):**

```cpp
case OrderState::Submitting:
    if (to == OrderState::Accepted ||
        to == OrderState::Rejected ||
        to == OrderState::Ambiguous ||
        to == OrderState::PartialFill ||  // NEW — symmetric to Filled
        to == OrderState::Filled)
        return TransitionResult::Ok;
case OrderState::Accepted:
    // existing PartialFill/Filled/CancelRequested/Expired, plus round-12:
    if (to == OrderState::Cancelled) return TransitionResult::Ok;
case OrderState::PartialFill:
    // existing Filled/CancelRequested/Expired, plus round-12:
    if (to == OrderState::Cancelled) return TransitionResult::Ok;
case OrderState::CancelRequested:
    // existing Cancelled/Filled/Expired, plus cancel-failure reverse edges:
    if (to == OrderState::Accepted || to == OrderState::PartialFill)
        return TransitionResult::Ok;
```

**`AbortedPreSend` removed — fixes this round's P0 (a dead, contradictory state).** An earlier revision added `OrderState::AbortedPreSend` (reachable via `Submitting → AbortedPreSend`) for a compensation path that no longer exists: §3's actual gate flow now handles every gates-1–8 failure (rules-version mismatch, policy/endpoint checks, clock staleness, budget-reservation refusal, sign/format failure) by releasing `InFlightRegistry`'s registration while `OrderRecord::state` **never leaves `Intent`** — Gate 6's "in-flight registration" registers the COID in `InFlightRegistry` (a structure independent of `OrderRecord::state`) but does **not** itself transition state; the `Intent → Submitting` transition happens exactly once, at Gate 9, the instant `OrderSubmitPrepared` is durably `.acked()` — the moment this design actually defines as "authorized to begin transport" (§3, Gate 9's own text). Since gates 1–8 can therefore never leave the order in `Submitting` in the first place, a `Submitting → AbortedPreSend` transition has no code path that could ever legitimately trigger it — it was vestigial from before the Prepared-boundary redesign (revision 13), not deleted when that redesign superseded it. Removed: the enum value, the `Submitting → AbortedPreSend` transition, `is_terminal(AbortedPreSend)`, and its inclusion in `is_exchange_final()` (§6.1.3 — reverted this round; it never should have needed to exist there once the state itself doesn't exist). A gates-1–8 failure's compensation is simply: release `InFlightRegistry`'s slot, leave the durable audit history at Intent-only (Gate 8's existing text), and the COID may be retried fresh — no new terminal state required for a case that, by construction, never touched `Submitting`.

One durable outcome record with `resulting_state = PartialFill` and fill fields populated is both live-legal and recovery-legal. Do **not** emit a synthetic intermediate `Accepted` frame for this path. Same-state open-order observations (`Accepted`+`NEW`, `CancelRequested`+`PENDING_CANCEL`, progressive `PartialFill`) must **not** call `transition_to()` — see L4 §6.6 observation matrix.

**Progressive fills after that first frame — fixes round-10 P0:** subsequent poll observations of the *same* `PARTIALLY_FILLED` status with a larger `executedQty` (L4 §6.6) are **not** state transitions and add no state-machine edge: the actor durably appends a fill-progress frame (`AuditEventType::OrderPartialFill`, `resulting_state = PartialFill`, new *cumulative* fill fields) under §6.4's ACK-before-apply discipline, then updates `OrderRecord`'s fill fields in place — `transition_to()` is never called for same-state progress, so `validate_transition()` needs no `PartialFill → PartialFill` self-loop. Replay accepts these frames under §6.1.4's monotonic rule (fills only grow, capped by intended qty). A *shrinking* `executedQty` from the exchange is treated as a schema anomaly (`Inconclusive`, never applied) — cumulative executed quantity cannot decrease for a single order.

**Principle**: default to `Ambiguous` unless a response is unambiguously, schema-validated interpretable as accepted or cleanly rejected *before* the matching engine — and "before the matching engine" is provable only for responses that occur before any network call is made (`StaleRulesVersion`) or for HTTP-level conditions L4/this spec have positively confirmed occur pre-matching-engine (§4.4's allowlist). Everything else, including `RateLimited`, defaults to `Ambiguous`.

### 4.4 HTTP-level condition → outcome mapping

| Condition | `SubmitOutcome` | Notes |
|---|---|---|
| HTTP 200, passes §4.2's full schema + field-match validation | `Accepted` | Branch then selected by `status` per §4.2/§4.3 — never by `fills[]` presence. |
| HTTP 200, but body fails to parse, schema mismatch, or any of §4.2's cross-checked fields (`symbol`/`clientOrderId`/`side`/`price`) doesn't match the sent request | `NetworkError` → `Ambiguous` | A 200 status code alone is never sufficient — the payload must positively identify *this exact* order. |
| HTTP 400 with a code from §4.4.1's allowlist below | `Rejected` | Order never reached matching-engine acceptance — but only for the specific, enumerated codes; anything else defaults to the next row. |
| HTTP 400 with any code NOT on §4.4.1's allowlist (fixes round-3 P1 — "e.g." was not exhaustive) | `NetworkError` → `Ambiguous` | Fail closed on unfamiliar codes rather than guessing they're safe to treat as a clean rejection. |
| HTTP 400 with `-1021` | `NetworkError` → `Ambiguous` + forced clock resync (§4.5, **rewritten round 10**) | A **post-send** condition. Routes to `Ambiguous` and reconciles via L4 §6 exactly like every other post-send uncertainty — **never re-POSTed** (rounds 4–9's "one audited retry with the same COID" assumed the original send provably never created an order; no HTTP response can prove that, §4.5). The one `-1021`-specific side effect is forcing a clock resync before any *future* signed request. |
| HTTP 401 (**new row, fixes round-6 P1**) | `NetworkError` → `Ambiguous` | Binance documents 401 as an API-key/signature authentication failure — but per this same table's own 403 reasoning (below), an authentication failure detected at a proxy/gateway layer is not documented as a hard guarantee against having reached order processing in every deployment shape. Treated identically to 403: no positive proof of pre-matching-engine rejection, so `Ambiguous`, not `Rejected`. |
| HTTP 404 (**new row**) | `NetworkError` → `Ambiguous` | Not a documented Binance response for a well-formed `POST /api/v3/order` — if seen at all, indicates something is wrong with routing/endpoint construction (or an intermediary), not a clean per-request rejection Binance itself issued. Fail closed rather than assume anything about order state. |
| HTTP 405 (**new row**) | `NetworkError` → `Ambiguous` | Same reasoning as 404 — not a documented per-order outcome. |
| HTTP 409 (**new row**) | `NetworkError` → `Ambiguous` | Not part of Binance's documented spot order-placement response set; as with 404/405, no basis for treating this as a clean rejection. |
| HTTP 403 (fixed round-5 P0 — was incorrectly `Rejected`) | `NetworkError` → `Ambiguous` | Binance documents 403 as a WAF/security block, but never as a guarantee that the request never reached order processing — the same reasoning round 3 applied to `RateLimited` and this spec's own §4.4's-neighbor `-1021` fix (§4.5): an HTTP response existing at all only proves the request left this process, not where it stopped. Treating 403 as a safe, definitive `Rejected` was an unverified assumption, not a documented Binance guarantee. Reconciled via L4 §6 like any other `Ambiguous` order; does **not** release in-flight. |
| HTTP 429 (fixed round-3 — was incorrectly `RateLimited`-as-never-submitted) | `RateLimited` → routes to `Ambiguous` (§4.3) | Parse `Retry-After` via L4 §7.3's `parse_retry_after()` (fixes round-5/6 P1 — missing/malformed → 429 uses IP-rollover+pad, never 60s; 418 → permanent fence; long real ban never clamped short), call `freeze_all_until()` on `RequestWeightTrackerSet` + `RawRequestsTrackerSet` + `OrderCountTrackerSet` (L4 §7.2) — freezes **all** sends. Does **not** release in-flight; reconciled via L4 §6 like any other `Ambiguous` order. |
| HTTP 418 (IP auto-ban) | `RateLimited` → `Ambiguous` | Same freeze-all-sets path as 429. Present `Retry-After` honored exactly (multi-day bans never clamped). **Missing/malformed `Retry-After` → permanent operator fence** (`source==2`, L4 §7.3) — never a 60s default. |
| HTTP 5xx (500/502/503/504), including error code `-1007` (matching-engine timeout) | `NetworkError` → `Ambiguous` | The request may or may not have reached the matching engine — must reconcile via L4 §6. |
| Any other HTTP status not explicitly listed in this table (**new row, fixes round-6 P1 — "exhaustive" now means every unlisted status too, not just every 400 sub-code**) | `NetworkError` → `Ambiguous` | A status this spec has no documented mapping for is, by definition, not something it can positively classify — defaults to the same fail-closed bucket as every other unproven case. |
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

**Removed from this list — fixes round-6 P0**: `-2010` (`NEW_ORDER_REJECTED`) was previously classified `Rejected` here on the reasoning "order-validation rejection, not matching-engine-state-dependent." That's not a safe generalization: Binance documents `-2010` as a **generic** matching-engine-adjacent rejection code covering multiple distinct conditions, including cases that can arise from a **duplicate `clientOrderId`** — meaning a `-2010` response is not always proof this specific submission attempt never resulted in a resting order; it can occur precisely *because* an order under this same client ID already exists (e.g. from an earlier attempt this process lost track of). Treating `-2010` as `Rejected` would release the in-flight slot and stop tracking an order that might, in that specific case, actually be live. `-2010` is **not** on the allowlist — it falls through to the default row below, routing to `Ambiguous` and reconciling via L4 §6, which can positively confirm one way or the other by querying the same `clientOrderId`.

Any code not on this list — including `-2010` and any future Binance code this list hasn't been updated for — defaults to `Ambiguous`.

### 4.5 `-1021` on the POST path — no re-POST, ever (fixes round-10 P0; supersedes rounds 4–9's "one audited retry")

Rounds 4–9 evolved `-1021` handling into "force a resync, then exactly one audited retry with the same `newClientOrderId`," justified by "`-1021` is a request-validation-layer rejection, so the original send did not create a resting order." Round 10 correctly identified that justification as self-contradictory with this spec's own §4.4 framing: **an HTTP response existing at all only proves the request left this process — it does not prove where inside (or in front of) Binance's stack it stopped.** A `-1021`-shaped 400 emitted by a proxy/CDN/edge layer, a response mis-attributed under connection reuse, or any spoofed/damaged intermediary response would make "the original can't exist" an assumption — and a same-COID re-POST on a wrong assumption is not idempotent-safe either, because Binance rejects a duplicate `newClientOrderId` with `-2010` *only if* the first order still exists in a live state; a first order that was created and then filled/cancelled in the gap can allow the second POST to create a **second real order**. That is the duplicate-submission bug this entire design exists to prevent, reachable through the one path that claimed to be safe.

Fixed handling — `-1021` is now *mechanically identical* to every other post-send uncertainty, plus one side effect:

1. **Clock side effect (kept)**: force a resync (`GET /api/v3/time`, L4 §2.2) before any *future* signed request, and record the observation via a diagnostic audit event (`AuditEventType::OrderClockDesyncObserved`, replacing the retired `OrderClockResyncRetried`). The stale offset is real information about the *next* request's signing; it is not license to re-send *this* one.
2. **This order routes to `Ambiguous`** (§4.3's standard row): stays in `InFlightRegistry`, reconciled via L4 §6's `GET /api/v3/order` by `origClientOrderId` — which answers the only question that matters ("does an order under this COID exist?") with evidence instead of an assumption. `Found` → confirmed state per L4 §6.5; `-2013` (order does not exist) folds into the existing `Inconclusive`-vs-confirmed logic L4 §6.1 already defines; attempts exhausted → escalate per §6.5's ledger.
3. **There is no re-POST path.** Not for `-1021`, not for anything post-send. If reconciliation *proves* no order exists under this COID (a positively-confirmed absence, not `Inconclusive`), the intent is surfaced to the strategy layer as failed — a *new* intent with a *new* COID may then be created through the full gate chain (Gates 1–14, new audit trail, new reservation). Order placement therefore has exactly one send site: Gate 8→10. No second-send code path exists to audit, reserve, or get wrong.

Consequences elsewhere: §3's Gate 8 note about "-1021 retry re-enters here" is deleted; L4 §7.2's `try_reserve_all_budgets()` is called from exactly one place; `AuditEventType::OrderClockResyncRetried` is not added to `audit_trail.hpp` (never shipped — it existed only in prior revisions of this document).

## 5. Reconciliation wiring

Any `Ambiguous` outcome (§4.3 — including `Timeout`, `NetworkError`, and, after round-3's fix, `RateLimited`) routes to L4 §6.2's orchestrator-level reconciliation loop, which calls `order_lifecycle.hpp::determine_reconcile_action()` to decide `QueryOrder` vs. `EscalateToOperator`, calling `BinancePrivateRestClient::query_order()` (§2) — a single attempt per scheduled task invocation — for the former. **Fixes round-8 P1**: an earlier sentence here said `query_attempts` was incremented and durably persisted *after* each attempt; that contradicts L4 §6.2 (persist *before* the GET, so a crash-loop cannot send unbounded real queries while the reconstructed count never advances). This file defers entirely to L4 §6.2's ordering — never restate a conflicting sequence here.

L4's revision 3 does propose one change to the state machine: `order_lifecycle.hpp::validate_transition()`'s `Ambiguous` case is extended (L4 §6.5) to permit transitioning into the real confirmed state (`Accepted`/`PartialFill`/`Filled`/`Rejected`/`Cancelled`/`Expired`) rather than forcing every `Found` result through the terminal `Reconciled` state, which would incorrectly mark a still-open order as done. This is the one place across both specs where an existing state machine's transition table needs an actual code change, not just a new caller wired into it.

## 6. Persistent audit — hard precondition, not deferred

`AuditRingSink` (`audit_trail.hpp`) is explicitly an in-memory, test/first-path sink (F5 fix made it fail-closed *once full*, but it still doesn't survive a process crash). Revision 2 named this as a precondition but defined no interface, no ACK contract, no recovery path — and a direct read of `live_submit_orchestrator.hpp` confirms **every one of its 16 `ctx.audit->append(ar)` call sites ignores the return value**. The "audit confirmed before POST" claim was not actually true of the current code; this revision fixes that gap concretely, not just in prose.

### 6.1 `DurableAuditSink` interface (proposed — replaces/extends `AuditRingSink` for L5 use)

```cpp
// FIXES ROUND-10/12 P0, EXTENDED ROUND-9 (this round) — literal compile gap:
//   - AuditAppendResult / RecoveryScanStatus: ONLY in
//     hengyuan/durable_control_plane.hpp (L4 §10).
//   - kMaxInFlight: ONLY in hengyuan/order_lifecycle.hpp (existing value 64).
//     This file must NOT redeclare it. kMaxEscalated is the one new constant
//     introduced here / in the DurableAuditSink header.
//   - account_truth.hpp: this file's own header sketches (OrderExpectation,
//     RecoveredOrderRecord, SubmitPort::SubmitFn, submit_order(), etc.) use
//     SymbolRules / OrderSide / OrderType pervasively but never included the
//     header that defines them — as written, none of this compiles. Fixed:
//     added below, alongside the two includes already present.
#include "hengyuan/account_truth.hpp"     // SymbolRules, OrderSide, OrderType,
                                            // AccountSnapshot, ExposureLimits
#include "hengyuan/durable_control_plane.hpp"
#include "hengyuan/order_lifecycle.hpp"  // kMaxInFlight, OrderState, …

inline constexpr std::size_t kMaxEscalated = 64;  // EscalatedLedger only
struct OrderRecoveryCheckpoint;

// CHANGED — fixes round-5 P0: round 4's RecoveredOrderRecord could carry
// state/fill-data/query_attempts, but still not enough to actually RESUME
// managing a recovered order: no side, no order type, no intended
// price/qty, no submit time, and no record of which SymbolRules snapshot
// (L4 §5.3) it was validated/submitted against. Without intended
// price/qty/side, a recovered order's eventual GET /api/v3/order response
// (L4 §6) can only be checked against origClientOrderId (§4.4's existing
// gap, fixed below) — not against the actual order this process believes it
// placed. Without price_scale/qty_scale at submit time, a recovered fill
// can't even be interpreted (ticks are meaningless without their scale).
// Every field below mirrors an existing OrderRecord field 1:1 — this
// remains "OrderRecord, but by value, for the recovery path," not a
// parallel/competing model:
struct RecoveredOrderRecord {
    ClientOrderId client_order_id{};
    std::uint32_t symbol_id{0};
    OrderState state{OrderState::Ambiguous};
    std::int64_t exchange_order_id{0};
    std::int64_t submit_timestamp_ms{0};        // NEW — mirrors OrderRecord::submit_timestamp_ms
    std::int64_t intended_price_ticks{0};       // NEW — mirrors OrderRecord::intended_price_ticks
    std::int64_t intended_qty_ticks{0};         // NEW — mirrors OrderRecord::intended_qty_ticks
    OrderSide side{OrderSide::Buy};             // NEW — was entirely absent; needed both to
                                                  // re-check a GET /api/v3/order response (L4
                                                  // §6, §4.2 below) and to resume balance/
                                                  // exposure accounting for this order
    OrderType order_type{OrderType::Limit};     // NEW — single-valued today, but recorded
                                                  // explicitly rather than assumed, so this
                                                  // struct doesn't silently break if OrderType
                                                  // ever gains a second value
    SymbolRules rules_snapshot_at_submit{};     // NEW — the L4 §5.3 snapshot this order was
                                                  // actually validated and formatted against;
                                                  // carries price_scale/qty_scale (required to
                                                  // interpret any tick value below) and
                                                  // rules_version (for audit/diagnostic purposes
                                                  // only — recovery never re-validates against
                                                  // a LIVE registry lookup, matching §2.2's
                                                  // TOCTOU fix: a recovered order is checked
                                                  // against what it actually saw, not against
                                                  // whatever the registry says post-restart)
    std::int64_t filled_qty_ticks{0};
    std::int64_t avg_fill_price_ticks{0};
    std::uint8_t query_attempts{0};  // L4 §6.2 — resume Ambiguous reconciliation
                                       // from here, never reset to 0 across a restart
    std::uint8_t open_poll_failures{0}; // L4 §6.6 — resume open-order status
                                          // polling; distinct from query_attempts
    std::int64_t last_poll_completed_utc_ms{0}; // hard-staleness baseline survives restart
};

// RecoveryScanStatus: defined ONLY in hengyuan/durable_control_plane.hpp
// (L4 §10) — see the include above; L5 attaches the following normative
// meanings for its order-recovery use (values unchanged, shared definition):
//   Clean            — log read successfully, zero in-flight orders found.
//                      Fixes round-5 P0: "0 recovered" vs "log unreadable"
//                      were previously indistinguishable.
//   Recovered        — log read successfully, 1+ in-flight orders written to
//                      out_records.
//   Corrupt          — checksum/MAC failure, structurally malformed record
//                      before log end, replay contradiction (§6.1.4), or
//                      store-identity/external-anchor mismatch (L4 §10.2) —
//                      MUST block L5 startup entirely.
//   CapacityExceeded — more genuinely-in-flight orders than kMaxInFlight can
//                      represent — MUST block startup (silent truncation
//                      could abandon a live order with no visibility).
//   IoError          — store unreadable (missing file, permissions, storage
//                      failure, breadcrumb-present-but-store-missing per L4
//                      §10.2) — MUST block startup; never treated as Clean.
//   ExternalAnchorUnavailable — production external tip channel unreachable
//                      at recovery (L4 §10.2); MUST block production startup
//                      unless a durable OperatorOverride evidence frame is
//                      present. Testnet may be configured to skip.

// Extends L4 §10 DurableControlPlaneSink (append_rate_freeze / append_snapshot
// / append_weight_config / append_usage_snapshot / recover_control_plane).
// Same physical store, same hash-chain / tip-anchor / fencing / breadcrumb /
// external-anchor contracts (L4 §10–10.3). Owner-actor thread only (§9 / L4 §9).
class DurableAuditSink : public DurableControlPlaneSink {
public:
    ~DurableAuditSink() override = default;

    // Blocking until durably persisted or failed. Never returns Acked before
    // the underlying storage confirms the write — which, per §6.1.1.1, ALSO
    // includes updating the persistent chain-tip anchor for the new tip.
    // On Acked: the frame AND its tip-anchor update are both durable.
    // On Failed (including flush-deadline expiry): the writer is FENCED
    // (§6.1.1.1) — no further append_* calls may succeed; L5 must stop.
    virtual AuditAppendResult append_durable(const AuditRecord& rec) noexcept = 0;
    virtual AuditAppendResult append_order_checkpoint(
        const OrderRecoveryCheckpoint& checkpoint) noexcept = 0;

    // Called once at process startup, before any order processing begins.
    // Replays the durable log and reconstructs full per-order state (not
    // just registry membership) for every order that was still in-flight
    // when the process last exited or crashed. Fixed-capacity output
    // buffer — no heap allocation, matching InFlightRegistry's own
    // kMaxInFlight bound.
    //
    // Writes 0..kMaxInFlight entries into out_records and returns how many
    // via out_count, but the STATUS return value is what the startup path
    // must branch on: only Clean/Recovered permit L5 to proceed. Corrupt,
    // CapacityExceeded, and IoError all fail closed — L5 does not start
    // until an operator resolves the underlying issue (ADR-019 D10's "audit
    // unavailable → no new orders," applied to the recovery side of the
    // same interface). The caller is responsible for both repopulating
    // InFlightRegistry from out_records AND restoring each corresponding
    // OrderRecord's full state (including rules_snapshot_at_submit) before
    // any reconciliation resumes.
    //
    // CRITICAL caller responsibility (fixes round-7 P0): any out_records
    // entry whose reconstructed `state` is `Submitting` MUST be stored into
    // the resumed OrderRecord as `Ambiguous`, not literally `Submitting`.
    // A recovered "Submitting" state means the durable log's last ACKed
    // event for that order was OrderSubmitPrepared with no later outcome event
    // ever ACKed — by definition the same "sent, outcome unconfirmed"
    // situation Ambiguous exists to represent. order_lifecycle.hpp::
    // determine_reconcile_action() only returns QueryOrder for state ==
    // Ambiguous (Submitting maps to NoAction) — restoring a recovered order
    // literally as Submitting would make it permanently unreachable by
    // reconciliation, identical to the in-process bug §6.4 fixes. States
    // other than Submitting (Accepted/PartialFill — a real, ACKed
    // confirmation existed before the crash) are restored as-is; only
    // Submitting is remapped.
    // Round-11/12 P0: EscalatedLedger recovery was described in §6.5 but
    // missing from this ABI — a restart could not rebuild ledger membership
    // or COID-reuse protection for escalated orders. Both arrays are
    // required; either overflowing its capacity → CapacityExceeded.
    virtual RecoveryScanStatus recovery_scan(
        std::array<RecoveredOrderRecord, kMaxInFlight>& out_inflight,
        std::size_t& out_inflight_count,
        std::array<RecoveredOrderRecord, kMaxEscalated>& out_escalated,
        std::size_t& out_escalated_count) noexcept = 0;
};
```

A recovered entry whose last ACKed event is `OrderEscalated` (no later `OrderOperatorResolved`) lands in `out_escalated`, never in `out_inflight`. `Submitting` remapping (§ above) applies only to the in-flight array. `kMaxInFlight` comes from `order_lifecycle.hpp` (include above); `kMaxEscalated` is defined once with this interface — never a second `kMaxInFlight` in this file.

#### 6.1.1 Durable record framing — version, length, checksum, truncated-tail recovery (fixes round-5 P0)

`recovery_scan()`'s Corrupt/IoError distinction (above) is only meaningful if the durable log format itself defines what "corrupt" means. This spec doesn't choose a storage backend (§7 — that's deliberately deferred), but it does fix the contract any backend's `recovery_scan()` implementation must uphold:

- **Every durable record is self-describing**: a format version byte, a record-type byte (§6.1.2), a **time-provenance byte** (`FrameTimeKind`, below), a **recording timestamp** (`recorded_utc_ms`, `std::int64_t`), a length prefix, the serialized payload, and a checksum covering `format_version || record_type || sequence_number || time_kind || recorded_utc_ms || length || payload` (§6.1.1.1 — HMAC-SHA256; both provenance and timestamp are MAC-covered).

```cpp
// CANONICAL definition: hengyuan/durable_control_plane.hpp (L4 §10).
// This file #includes it; a second definition here is an ODR defect.
enum class FrameTimeKind : std::uint8_t {
    ServerCorrectedUtc = 0, // recorded_utc_ms = local_utc + published offset
    UnknownBootstrap   = 1, // recorded_utc_ms MUST be 0
};
```

**Provenance rules (fixes round-19 P0 + round-20 P0 — closed allowlist, not "freeze only"):**

Default: every append uses `ServerCorrectedUtc` with `recorded_utc_ms` from L4 §2.2's published snapshot. Missing/stale offset → **refuse** the append (fail-closed), **except** the closed set below.

`UnknownBootstrap` (`recorded_utc_ms = 0`) is legal **only** for these record types, and only while no trustworthy clock offset is published (or, for probes, while the active freeze episode was itself begun under UnknownBootstrap and the clock is still unpublished):

| `DurableRecordType` | When `UnknownBootstrap` is legal |
|---|---|
| `FreezeEpochWatermark` | Front-half of any **new** freeze episode begun while no clock is published (kind may be `source` 0/1/2/3 — L4 §7.3). Merges do not write a watermark. |
| `RateLimitFreeze` | Any freeze kind persisted before clock publish: `source=3`; `source=2` permanent; **or** `source` 0/1 with present Retry-After via **this event's** `conservative_wait_ms` contribution (including **merge** frames). Round-23: a merge under `UnknownBootstrap` **must copy-forward** any prior non-zero `deadline_utc_ms` in the payload (header Unknown does not authorize zeroing UTC). Round-31: wait-bearing events advance `wait_generation`; wait contribution `0` copy-forwards gen (do not restate folded max wait into the frame). Live `append_rate_freeze` only. |
| `RateLimitFreezeSnapshot` | Compaction-only folded aggregate (`append_compacted_freeze_snapshot`); UnknownBootstrap legal when rewriting an episode that began under Unknown and clock still unpublished. Not a live event contribution. |
| `FreezeProbeAttempt` | Attempt (`cleared` MUST be false) for an epoch whose freeze was `UnknownBootstrap`, clock still unpublished. Round-26/28/29: Ack is the budget linearization point — `/time` send illegal until Acked. Includes `ClockRepublishOrVerify` purpose (same 8-cap; may `ProbeVerified`-clear when deadline past). |
| `FreezeWaitArm` | Arm a conservative wait for an UnknownBootstrap / dual-deadline episode while clock unpublished; binds current `wait_generation`; does not satisfy or clear. Live `append_freeze_wait_arm` only. |
| `FreezeWaitSatisfied` | Non-terminal wait evidence citing a live-session Arm for the **current** `wait_generation`; sink-verified elapsed; does not clear. Stale-generation Satisfy is not evidence. Live path only. |
| `CompactedFreezeWaitEvidence` | Compaction-only **single** frame with `CompactedFreezeWaitEvidencePayload` (`append_compacted_wait_evidence`); embeds Arm+Satisfy + source seqs + baseline tip bind; no `arm_ack_steady`; UnknownBootstrap legal under the same clock-unpublished episode rules. Dual live Arm+Satisfy frames are **not** a legal compaction substitute. |
| `FreezeClear` | `ProbeVerified` / `ConservativeWaitCompleted` for an UnknownBootstrap non-permanent episode while clock still unpublished — but `ProbeVerified` still requires a verifiable `FreezeTimeProbeProof` once a UTC deadline exists (prefer ServerCorrectedUtc for that clear), and when `conservative_wait_ms > 0` also requires a prior sink-verified `FreezeWaitSatisfied` for the **current** `wait_generation` (same for `ConservativeWaitCompleted` when wait>0). **`OperatorAuthorized` requires `ServerCorrectedUtc`**. |

All other types (`OrderEvent`, `SymbolRegistrySnapshot`, `GenerationBridge`, `OrderCheckpoint`, `EndpointWeightConfigSet`, `RateLimitUsageSnapshot`, `OperatorOverride` mirror, `TransportFailover`, …) **never** use `UnknownBootstrap` — missing clock → refuse / defer that append.

**Seal-journal provenance (round-43 P0 — L4 §10.1):** Path B journal entries and `SealJournalAppliedView` **must** persist the observation's original `time_kind` + `recorded_utc_ms` (same rules as this section). Apply/replay via `append_seal_journal_apply(view)` writes the store-frame header from the view — **never** from the recovering process's current clock. `UnknownBootstrap` journaled freeze/wait/probe frames remain `UnknownBootstrap` + `recorded_utc_ms==0` even if a clock is published before replay; `ServerCorrectedUtc` journaled frames keep their original UTC even if the clock is later lost. Hard-lag age on applied frames uses that preserved `recorded_utc_ms` (Unknown still excluded from age). A separate `FrameTimeKind` argument on `append_seal_journal_apply` is **forbidden**.

**Same-episode invariant (normative):** for one new freeze allocation, watermark and freeze share one `FrameTimeKind`. Merges append another freeze on the **same** epoch with no watermark. Callers pass provenance via ABI:
- `append_freeze_epoch_watermark(next, time_kind)` — new episodes only
- `append_rate_freeze(payload, time_kind)` — live event contribution only
- `append_compacted_freeze_snapshot(folded, time_kind, proof)` — compaction into gen-N+1 only
- `append_freeze_probe_attempt(attempt, time_kind)` — `attempt.cleared == false`; Ack before `/time` send
- `append_freeze_wait_arm(arm, time_kind)` — live arm before wait
- `append_freeze_wait_satisfied(wait, time_kind)` — live non-terminal wait evidence (sink elapsed check)
- `append_compacted_wait_evidence(evidence, time_kind, proof)` — compaction retain/rewrite only (single payload)
- `append_freeze_clear(clear, time_kind)` — sole terminal clear
- `append_seal_journal_apply(view)` — provenance **inside** `SealJournalAppliedView` only (no separate `time_kind` arg)

Mixing `ServerCorrectedUtc` watermark with `UnknownBootstrap` freeze (or authorizing freeze while refusing watermark on a **new** episode) is a defect.

- **Age / TTL / hard-lag ms**: only `ServerCorrectedUtc` frames participate. `UnknownBootstrap` frames are excluded from age computations; they still count toward **frame-count** soft/hard lag (L4 §10.2.1). Recovery must not invent an age for Unknown frames.
- `append_durable()` / control-plane `append_*` compute and write the checksum as part of the same durable write they Ack — a record is never `Acked` without its checksum in that write.
- **Scanning is sequential from the start of the log.** For each record, first check **physical presence**: does the length prefix's claimed end fall within the bytes actually present in the file? Only if a record is NOT fully physically present does the "torn write" exception below apply — this distinction is what round 5's rule got wrong (see below).
- **Fixes round-6 P0**: round 5's rule said a checksum failure on "the last record" is safe to discard as a torn write, regardless of *why* it failed. That conflates two different failure shapes that must be treated differently:
  - **Truncated (not fully physically present)** — length prefix claims more bytes than actually exist before EOF. This is the one genuinely expected, non-corrupt failure mode: a crash mid-`append_durable()` leaves a partially-written final record (the OS/filesystem never guaranteed that write was atomic, even though `append_durable()`'s caller never observed it as `Acked`). Recovery **discards only this incomplete tail record** and proceeds with everything before it. This case can, by construction, only ever occur on the physically-last record in the file — a torn write cannot leave a gap in the *middle* of a file that's still followed by more valid-looking bytes.
  - **Complete but checksum-mismatched** — every byte the length prefix claims is physically present, but the checksum over those bytes doesn't match. This is **NOT** a torn write (the write completed; nothing was left half-finished) — it's evidence of bit-level corruption after the fact (media failure, filesystem bug, disk error), and critically, **this can happen to the last record just as easily as any other**, including a record that was genuinely `Acked` before the corruption occurred (e.g. a fully-written, fsync'd `OrderSubmitted` whose sectors later degrade). Silently discarding a complete-but-corrupt record — round 5's actual rule — could discard evidence that an order really was submitted, letting recovery believe it never happened and risking a duplicate submission. **A complete frame with a checksum mismatch is always `RecoveryScanStatus::Corrupt`, regardless of its position in the log** — never silently dropped.
- Any `Corrupt` classification is intentionally conservative: recovery does not attempt "skip the bad record and keep scanning," since a bad record's length field, if untrustworthy, could point anywhere in the remaining bytes, and continuing to parse untrusted offsets risks either fabricating an order that never existed or silently skipping one that did. Fail closed, block L5 startup, let an operator inspect the log directly.

#### 6.1.1.1 Canonical byte encoding, single-writer discipline, flush deadline, and tamper evidence — fixes round-6 P1

§6.1.1 defined the *shape* of a frame (version, type, length, payload, checksum) but left several properties unstated that any two independent implementations (or even the same implementation across a version upgrade) would need to agree on to interoperate at all:

- **Endianness and padding**: all multi-byte integer fields (length prefix, `rules_version`, `timestamp_ms`, etc.) are written **little-endian**, matching this codebase's target platforms (x86-64 on both GCC/Linux production and MSVC/Windows dev, per `CLAUDE.md`) — chosen for zero-cost `memcpy`-style encode/decode on the only architectures this project actually runs on, not for cross-platform portability this project doesn't need. Struct payloads are written **field-by-field with an explicit encoding function per struct** (not a raw `memcpy` of the in-memory struct), specifically to avoid struct-layout padding becoming part of the durable format — padding bytes are compiler/ABI-dependent and must never leak into a format that has to remain readable across rebuilds.
- **Format version byte + migration (fixes round-19 P1)**: `recovery_scan()` dispatches on this byte to select a decoder. A version byte it doesn't recognize is **not** the same as a corrupt record — it means "written by a newer or differently-configured build than this one can decode," and is reported as `RecoveryScanStatus::IoError` (folded — this process cannot safely read its own audit log — rather than `Corrupt`, since nothing about the bytes themselves is wrong).

  | Version | Header layout (after `format_version`) | Status |
  |---|---|---|
  | `1` | `record_type \|\| sequence \|\| length \|\| payload \|\| prev_mac \|\| mac` (no wall time) | Decode-only legacy; treat missing time as `UnknownBootstrap` for age (frame-count lag only). |
  | `2` | `… \|\| recorded_utc_ms \|\| length \|\| …` (implicit `ServerCorrectedUtc`) | Decode-only; writers must not emit v2 after this revision. |
  | `3` (**current write version**) | `… \|\| time_kind \|\| recorded_utc_ms \|\| length \|\| …` | Sole version new appends may write. |

  **Migration rules (normative):**
  1. A process that writes format v3 may still **decode** v1/v2 frames in an existing generation (required until compaction migrates them).
  2. Once a process has written any v3 frame into a generation, it must not append further v1/v2 frames into that same generation (mixed post-upgrade write streams forbidden).
  3. Compaction to generation `N+1` **always** rewrites every retained frame as format v3 (current), then seals — that is the only operator path that retires the need to keep v1/v2 decoders for live CURRENT generations. Archived read-only generations may remain at their original version indefinitely.
  4. Silently writing a new format into a log an older-format-only reader might still need to scan as CURRENT is not acceptable — bump CURRENT only via the compaction seal path (L4 §10.2 / §10.3).
- **`enum` fields are range-checked on decode, always.** `AuditEventType`, `OrderState`, `OrderSide`, `OrderType`, `DurableRecordType`, **`FrameTimeKind`** are all backed by `std::uint8_t` — a corrupted or truncated byte can trivially decode to a bit pattern with no corresponding enum value. Every decode path explicitly validates the byte is one of the enum's defined values before constructing/using it; an out-of-range value is a `Corrupt` frame (§6.1.1), never `static_cast` into the enum type and used as if it were valid.
- **Single-writer discipline**: exactly one process may hold `append_durable()`-write access to a given durable log at a time — enforced via an OS-level advisory file lock (or the storage backend's equivalent — e.g. a single-writer transaction guarantee if the eventual backend is a DB, not a flat file) acquired at `DurableAuditSink` construction and held for the process's lifetime. Two processes writing the same log concurrently (e.g. an operator accidentally starting a second L5 instance against the same durable store) is not a race this design tries to make safe — it fails the second process's startup outright rather than silently interleaving writes from two writers, which no `recovery_scan()` implementation could be expected to make sense of afterward.
- **Flush deadline + writer fencing — fixes round-8 P1**: `append_durable()` / `append_snapshot()` / `append_rate_freeze()` already block "until durably persisted or failed." The underlying flush/`fsync`/`FlushFileBuffers` has a bounded deadline (matching L4 §8's per-phase network deadlines) so a hung storage device degrades to a defined `Failed` rather than blocking the L5 owner thread indefinitely. **Critical addition round 8 required**: a flush deadline is *not* a reliable cancellation of the OS write — the `fsync` may still complete *after* the caller has already observed `Failed`. Treating that late completion as a normal durable frame would create sequence-number / chain-tip ambiguity (the caller believes the append failed and may retry with a new sequence; the late write already occupies that sequence on disk). **Therefore, any flush-deadline `Failed` (and any other `Failed` from an append_* path) permanently FENCES the writer for this process lifetime**: subsequent `append_*` calls return `Failed` immediately without touching storage; L5 stops (no new submits, no reconciliation GETs that require durable attempt records). Recovery after operator intervention starts from a clean process with `recovery_scan()` deciding whether the ambiguous tail is a torn write (discard) or a complete frame (keep). There is no "retry the same append after Failed" path inside a fenced process.

- **Bounded group commit (round-13 I/O/P1):** the single owner actor may coalesce independently queued append requests into one durability barrier, using a preallocated fixed ring of `kAuditCommitQueue=256`, `kAuditCommitMaxFrames=32`, and `kAuditCommitMaxDelayUs=200`. ACK for every frame in the batch is returned only after the shared log + anchor durability barriers complete; order `Prepared` ACK still precedes that order's first transport write. The queue is FIFO, never heap-backed, and returns `Failed`/fences on overflow, deadline expiry, or device error—there is no silent drop or caller-side unbounded wait. Production acceptance must measure `p99 <= 2ms`, `p999 <= 10ms` from enqueue to ACK under the declared storage profile; exceeding either is a deployment failure, not a reason to increase queue limits blindly.
- **Tamper evidence — fixes round-7 P0; tip-anchor semantics tightened by round-8 P0.** Round 6 upgraded CRC→HMAC but left whole-frame deletion undetectable. Round 7 added full-frame MACs + hash chain + a persistent tip anchor, but the anchor was only written "at clean shutdown and periodically," which leaves every frame after the last periodic anchor unprotected: delete a trailing already-`Acked` `OrderSubmitted`, and the remaining chain tip still *matches* the stale anchor — recovery accepts the log and forgets the order. Round 8 also found that deleting/missing the anchor file itself had no fail-closed rule.

  **Fix — four changes, not three:**
  - **Full-frame MAC**: the HMAC-SHA256 covers `format_version || record_type || sequence_number || time_kind || recorded_utc_ms || length || payload` concatenated with `prev_mac` — every byte of the frame except the MAC field itself (format v3; older versions omit fields they never had, per the version table above).
  - **Monotonic sequence number + hash chain**: every frame carries a `std::uint64_t sequence_number` (strictly increasing by 1, no gaps, starting from 0 for a fresh log) and a `prev_mac` field containing the *previous* frame's full MAC (all-zero for `sequence_number == 0`). Each frame's own MAC is computed over its full-frame bytes (above) **concatenated with `prev_mac`**. Deleting frame N from the middle breaks the chain at frame N+1. A sequence-number gap is also checked explicitly and flagged `Corrupt`.
  - **Persistent chain-tip anchor updated on EVERY `Acked` append (fixes round-8 P0; `key_id` closed round-11 P1)**: the tip `{sequence_number, mac, key_id, store_uuid}` is written to a small, separate, HMAC-protected anchor location as part of the *same* durability barrier that makes `append_*` return `Acked` — not merely at clean shutdown / periodic intervals. The anchor's own MAC is computed with the **same `key_id` that signed the tip frame** (§6.1.1.2); after a key rotation, recovery selects the wrapper for that `key_id` from the retained history to verify the anchor — an anchor that omits `key_id` (or carries one with no retained wrapper) is `Corrupt`, never verified under "whatever the current key happens to be." Concrete ordering inside every successful append:
    1. Append the new frame bytes to the log.
    2. `fsync`/`FlushFileBuffers` the log (within the flush deadline).
    3. Write the new tip anchor (including `key_id`).
    4. `fsync` the anchor (L4 §10.3 atomic-replace contract).
    5. Only then return `Acked`.
    If step 3/4 fails after step 2 succeeded, return `Failed` and fence the writer — the log may be one frame ahead of the anchor; `recovery_scan()` treats "log tip extends past a readable anchor" as acceptable *only when the in-log chain verifies continuously from the anchored tip through the extra frames* (crash between step 2 and 4), and treats "log tip *behind* anchor" or "anchor MAC invalid" as `Corrupt`. Periodic/shutdown anchor writes are retained only as defense-in-depth, never as the sole tip-protection mechanism.
  - **Missing / unreadable / deleted anchor fail-closed (fixes round-8 P0)**:
    - Empty log + missing anchor → `Clean` (genuinely fresh store).
    - Non-empty log + missing anchor → `IoError` (indistinguishable from "anchor was deleted to hide a truncated tip") — L5 does not start.
    - Anchor present but MAC/decode failure → `Corrupt`.
    - Anchor tip ahead of log tip → `Corrupt` (tail deletion after the last Acked append).
    This does not make deletion physically impossible against an adversary who rewrites *both* log and anchor consistently, but it closes the "delete one trailing order frame and/or the anchor file" window round 8 demonstrated against the periodic-only design.

  The HMAC key itself is derived from — but distinct from — this process's existing Binance API secret material (never the raw API secret itself), reusing this codebase's existing OpenSSL HMAC-SHA256 infrastructure (`CLAUDE.md`'s own stated stack) rather than introducing a new cryptographic dependency. See §6.1.1.2 below for key rotation (fixes round-7 P1 — a real gap in this same area round 6 left unaddressed).

#### 6.1.1.2 HMAC key identity and rotation — fixes round-7 P1 + round-9 P1 (KEK / destruction)

Deriving the audit-log HMAC key from the Binance API secret (§6.1.1.1) has a real consequence round 6 didn't address: if that secret is ever rotated, every durable frame written under the old secret becomes unverifiable under the new one, unless the key derivation and rotation history are tracked explicitly. Round 8 retained raw derived key material in a side map with no wrapping or destruction rules — a plaintext key-history store. Fix:

- Every frame's key derivation includes a `std::uint32_t key_id` (MAC'd header), incremented on each API-secret rotation.
- **Tip-anchor selection protocol (round-11 P1)**: the tip-anchor record stores the same `key_id` as the tip frame. Verification: look up `key_id` in the wrapped history → unwrap under KEK → verify anchor MAC → verify chain from that tip. Never fall back to "try all keys" or "use current key only" — both are either DoS-shaped or rotation-blind.
- Derived HMAC keys are stored **only wrapped under a process Key-Encryption-Key (KEK)** — sourced from a platform secret store / DPAPI / file mode `0600` operator-provisioned KEK distinct from the Binance API secret. At rest in the metadata store: `{key_id, wrapped_key_blob, wrap_alg}` — never raw HMAC key bytes on disk.
- Access: only the owner-actor process may unwrap; no network export API; debug/logging must never print key material.
- Destruction: when compaction/archival retires a `key_id`, the wrapped blob is securely erased (overwrite + unlink) after the archived log that needed it is sealed read-only offline. Rotating the live API secret without retaining a wrapped mapping for in-log `key_id`s still fails closed (entire prior log unverifiable) — operators must run the retain-then-rotate procedure. The tip-anchor's `key_id` must remain retainable for at least as long as the live generation references it.
- Derivation binds `BinanceEnvironment` (L4 §1) so testnet keys never verify production frames.

#### 6.1.2 `AuditRecord` extension, and why a registry snapshot can't be one — fixes round-6 P0 (round 5's fix described a payload shape the data model has no room for)

`audit_trail.hpp`'s existing `AuditRecord` (`timestamp_ms`, `event_type`, `mode`, `symbol_id`, `client_order_id`, `exchange_order_id`, `price_ticks`, `qty_ticks`, `detail_code`, `detail_msg`) has no `side`, `order_type`, resulting `state`, or `rules_version` field — `recovery_scan()` cannot reconstruct `RecoveredOrderRecord::side`/`order_type`/`rules_snapshot_at_submit.rules_version` from a log that never recorded them in the first place. Round 5's fix added 4 fields to close that gap, but also proposed a `SymbolRegistryRefreshed` event carrying "the full `SymbolRules` for every affected symbol" **inside** this same fixed-size `AuditRecord` — that's not representable: `AuditRecord` is a single-symbol, fixed-field struct, and a registry refresh can affect every symbol in the registry (potentially hundreds) in one atomic operation (§5.1). There was nowhere in the struct for that payload to actually go.

```cpp
// audit_trail.hpp — AuditRecord gains the following fixed fields,
// used for per-ORDER events only:
struct AuditRecord {
    // ... existing fields unchanged ...
    OrderSide side{OrderSide::Buy};           // NEW
    OrderType order_type{OrderType::Limit};   // NEW
    OrderState resulting_state{OrderState::Intent}; // NEW — the state this
                                                       // event transitioned
                                                       // the order TO
    std::uint32_t rules_version{0};           // NEW — L4 §5.3's version at
                                                // the time of this event —
                                                // an INTEGER reference into a
                                                // separately-recorded registry
                                                // snapshot (below), never the
                                                 // snapshot's actual contents
    std::uint64_t event_sequence_ref{0};      // frame sequence that this event
                                                 // semantically acknowledges; 0 only
                                                 // for an originating event
    std::uint64_t reset_after_seq{0};         // poll-reset source frame sequence;
                                                 // nonzero only for reset events
    std::uint8_t query_attempts{0};
    std::uint8_t open_poll_failures{0};
    std::int64_t last_poll_completed_utc_ms{0};
};
```

**Canonical event/replay fields (round-13 P0):** `event_sequence_ref` is the durable sequence of the event being acknowledged; `reset_after_seq` is mandatory for an `OpenPollFailureReset` and must equal the preceding successful poll event's frame sequence. `query_attempts`, `open_poll_failures`, and `last_poll_completed_utc_ms` are written on every event which changes them; replay restores the latest values in sequence order. No field may be overloaded through `detail_code`/`detail_msg`. A nonzero reference must point to an earlier same-COID frame of the compatible event type, otherwise recovery is `Corrupt`.

```cpp
// L5-owned payload: L4 only owns its record-type number, preserving L4 -> L5
// dependency direction. Fixed size, canonical declaration-order encoding.
//
// CHANGED — fixes round-14's self-contradiction (renamed last_event_sequence
// to own_sequence) AND this round's P0 (own_sequence, as a PAYLOAD field,
// was unconstructible): a payload field is serialized and MAC'd as part of
// the write itself (§6.1.1.1's frame = version + type + sequence + length +
// payload + MAC) — but the sequence a write will be ASSIGNED is only known
// from that write's own AuditAppendResult, returned AFTER append_order_
// checkpoint() completes. Requiring the payload to already contain the
// sequence its own write will receive is circular, and worse under any
// future group-commit/batched-write implementation where the assignment
// genuinely cannot be predicted by the caller in advance. Fixed by removing
// the field entirely — it was redundant from the start: §6.1.1.1's frame
// HEADER already carries every frame's sequence number (that's the literal
// source `AuditAppendResult::sequence` and `DecodedOrderFrame::sequence`,
// §6.1.4, both already read from), so a checkpoint's own sequence is always
// available at decode time via `DecodedOrderFrame::sequence` — duplicating
// it inside the payload added nothing.
struct OrderRecoveryCheckpoint {
    RecoveredOrderRecord recovered{};
    std::uint32_t submit_rules_version{0};
    // own_sequence REMOVED (fixes this round's P0) — read the checkpoint
    // frame's sequence from DecodedOrderFrame::sequence at decode time
    // instead; nothing in the payload needs to carry it.
};
```

`OrderRecoveryCheckpoint` is written as `DurableRecordType::OrderCheckpoint`; it must contain all immutable expectation fields, the complete `RecoveredOrderRecord`, and its pinned rules version. "No reference to an earlier generation" is enforced by construction — nothing in this struct's fields *can* reference a sequence at all, old-generation or otherwise; the frame header (§6.1.1.1, decoded into `DecodedOrderFrame::sequence`) is the only sequence a checkpoint frame ever carries, and it is always fresh, assigned at the moment THIS write actually happens. A later same-COID event frame that needs to chain onto a checkpoint (`AuditRecord::event_sequence_ref`) uses the value `append_order_checkpoint()`'s own `AuditAppendResult.sequence` returned to the writer immediately after the checkpoint write completed — captured and threaded forward by the caller, exactly the same post-write-capture pattern already established for `OrderOpenPollFailuresReset::reset_after_seq` (L4 §6.6 rule 5) — never a value the checkpoint payload itself needs to predict or contain. Framing is payload-agnostic (L4 §10 + §6.1.1). L5 contributes `DurableRecordType::OrderEvent = 0` and `OrderCheckpoint = 8`; `SymbolRegistrySnapshot`, `RateLimitFreeze`, and related control-plane types are **canonical in L4 §10**. `DurableAuditSink` extends `DurableControlPlaneSink` and writes OrderEvent/checkpoint frames into the same store.

`recovery_scan()` reconstructs `rules_snapshot_at_submit` from `OrderIntentCreated`, `OrderSubmitPrepared`, or an `OrderRecoveryCheckpoint`, then locates the exact pinned `SymbolRegistrySnapshotPayload`. A complete Prepared frame is always recovered as `Ambiguous`; a checkpoint establishes its embedded state directly. `query_attempts`, `open_poll_failures`, and `last_poll_completed_utc_ms` come from canonical checkpoint/event payloads, never an old-generation sequence reference. Active freezes are restored before any signed send.

**The type §6.1.4's replay loop actually iterates over — fixes this round's P0 (undefined variant frame).** §6.1.4 below reads `record.record_type` and `record.checkpoint` — fields that exist on neither `AuditRecord` nor `OrderRecoveryCheckpoint` individually; no earlier revision ever defined the type that carries both. `recovery_scan()`'s sequential scan (§6.1.1.1) decodes each physical frame by its `DurableRecordType` byte into a small, explicit tagged union — not a literal C++ `union` (this codebase avoids those for anything with non-trivial members; a tagged struct with only the active member touched is the same zero-heap-allocation discipline `SymbolRegistrySnapshotPayload`'s fixed buffer already uses) — and *that* decoded value, not a raw frame, is what §6.1.4's replay loop iterates:

```cpp
// NEW — the actual type recovery's per-COID scan iterates. Fixed-size,
// stack/member-resident, no heap allocation.
struct DecodedOrderFrame {
    DurableRecordType record_type{DurableRecordType::OrderEvent};
    std::uint64_t sequence{0};       // this frame's own assigned sequence —
                                       // for BOTH record_type values, this is
                                       // read directly from the frame header
                                       // (§6.1.1.1) — the same value the
                                       // original write's AuditAppendResult
                                       // returned. Neither AuditRecord nor
                                       // OrderRecoveryCheckpoint carries its
                                       // own sequence as a payload field
                                       // (fixed this round: OrderRecoveryCheckpoint
                                       // used to, which was unconstructible —
                                       // the header is the only source now,
                                       // for every frame type uniformly)
    std::int64_t recorded_utc_ms{0}; // from frame header; 0 when
                                       // time_kind == UnknownBootstrap
    FrameTimeKind time_kind{FrameTimeKind::ServerCorrectedUtc};
                                       // NEW — round-19 P0: provenance;
                                       // UnknownBootstrap must not drive
                                       // age/TTL/hard-lag ms (L4 §10.2.1)
    AuditRecord event{};              // valid iff record_type == OrderEvent
    OrderRecoveryCheckpoint checkpoint{}; // valid iff record_type == OrderCheckpoint
};
```

§6.1.4's replay loop iterates `DecodedOrderFrame` values for a given `client_order_id` (matched via `event.client_order_id` or `checkpoint.recovered.client_order_id` depending on `record_type`), in `sequence` order; `record.record_type`, `record.checkpoint`, and `record.event` below refer to this type's fields, not to any field imagined to exist directly on a raw durable frame.

#### 6.1.3 Bounding the variable-length path — fixes round-7 P0 (a corrupted `symbol_count`/length field could OOM or hang recovery, the exact class of bug §6.1.1's fixed-size framing didn't have to worry about)

`SymbolRegistrySnapshotPayload` is this design's only variable-length frame — every other frame (`AuditRecord`) is fixed-size, so `recovery_scan()`'s allocation and arithmetic never had to defend against a corrupted length driving unbounded memory use or an overflowed offset calculation. That changes here, and round 6 never stated the bounds:

- **`symbol_count` is capped at `kMaxSymbols`** (the same compile-time bound `SymbolRegistry::table_` is already sized to, L4 §5.3) — checked **before** any allocation or read of the entries that follow, not discovered partway through. A frame claiming `symbol_count > kMaxSymbols` is `RecoveryScanStatus::Corrupt` immediately — this can never be a legitimate registry snapshot, since the live registry itself cannot hold more than `kMaxSymbols` entries.
- **No heap allocation for the entries buffer** — matching this codebase's zero-heap-allocation discipline (`CLAUDE.md`), `recovery_scan()` decodes directly into a fixed `std::array<SymbolRules, kMaxSymbols>` stack/member buffer sized once, never a `symbol_count`-driven `std::vector`/`new[]`. Combined with the `symbol_count <= kMaxSymbols` check above, this makes an over-large `symbol_count` a bounds-check failure, never an allocation-size input.
- **All offset/length arithmetic during scanning is checked, not assumed in-range.** Computing "where does this frame's payload end" as `offset + length` (or `header_offset + symbol_count * sizeof(SymbolRules)` for this payload specifically) uses overflow-checked addition/multiplication (the same discipline `account_truth.hpp`'s `checked_notional()`/`checked_add()` already establish for financial arithmetic, applied here to file-offset arithmetic reading an untrusted, potentially-corrupted file) — an operation that would overflow `std::size_t` is `Corrupt`, never silently wrapped into a small or negative-looking offset that could then be read from or compared against out-of-bounds.
- **The rules_version → snapshot index itself is fixed-capacity.** `recovery_scan()`'s join (above) needs to look up "the `SymbolRegistrySnapshotPayload` frame for rules_version N" — this index is bounded to a fixed maximum number of distinct registry versions trackable during one recovery pass (`kMaxTrackedRegistryVersions = 64`). Exceeding it is `RecoveryScanStatus::CapacityExceeded` (the existing enum value, §6.1's `RecoveryScanStatus` — reused, not a new case), not an unbounded in-memory map.

**Refresh cadence, compaction, and pin rules — fixes round-8 P1** (round 7 named the bound but left long-running processes able to self-deadlock at the next restart):

- **Refresh cadence**: `SymbolRegistry::refresh_from_exchange_info()` is operator-triggered and/or scheduled on the L5 actor (§9.1) at a minimum interval of `kMinRegistryRefreshIntervalMs` (default 1 hour) — not on every account poll and not inline with every submit. A refresh that would push the number of distinct durable snapshot versions since the last compaction above `kMaxTrackedRegistryVersions` is **rejected** (`refresh_from_exchange_info()` returns `false`, in-memory table unchanged) until compaction succeeds — never silently overwrite or drop an old snapshot that recovery might still need.
**`is_exchange_final()` vs `is_terminal()` — fixes this round's P0 (a real, confirmed retention-rule inconsistency).** Bullet 1 below correctly uses "non-exchange-final" as its retention criterion, but the original bullet 3 (and the "pin rule" paragraph after it) used "non-terminal," borrowed from `order_lifecycle.hpp::is_terminal()`. A direct check of that function's actual definition shows `is_terminal(OrderState::EscalatedToOperator) == true` — `EscalatedToOperator` IS terminal in the state-machine sense (no further automatic transition ever leaves it), but it is emphatically **not** safe to treat as "done, snapshot no longer needed": an escalated order still awaits operator resolution, and an operator who finally looks at it still needs `price_scale`/`qty_scale`/`quote_scale` to make sense of what they're looking at. Using `is_terminal()` as the snapshot-retention filter would let compaction drop the pinned `SymbolRegistrySnapshot` for an escalated-but-unresolved order — silently corrupting `recovery_scan()`'s ability to interpret it, forcing `Corrupt`/`CapacityExceeded` on the next restart. Fixed: a dedicated `is_exchange_final()` predicate, used consistently everywhere compaction decides what's safe to drop:

```cpp
// order_lifecycle.hpp — NEW, distinct from the existing is_terminal():
// "safe to stop retaining checkpoint/snapshot context for this COID —
// the exchange has definitively finished with it" — excludes
// EscalatedToOperator (is_terminal() includes it; still awaits operator
// follow-up, which needs retained context). CHANGED this round: no longer
// mentions AbortedPreSend — that state has been removed entirely (§4.3
// above), since the gate flow that would have produced it never actually
// leaves OrderRecord::state at Submitting for a pre-send failure in the
// first place (release InFlightRegistry, stay at Intent — no terminal
// state needed).
//
// CHANGED AGAIN this round — fixes a genuine internal contradiction the
// review caught (P0): this definition included `Reconciled`, but §6.5's
// later, authoritative statement ("is_exchange_final returns true only for
// Filled, Cancelled, Rejected, and Expired") does not — and §6.5 is
// correct. `Reconciled` is `order_lifecycle.hpp`'s pre-existing enum value,
// but no code path this design proposes ever transitions INTO it as a live
// target: round 3's fix (L4 §6.5) made `Found` map to the real confirmed
// state (Accepted/PartialFill/Filled/Rejected/Cancelled/Expired), never to
// a blanket `Reconciled`; `OrderOperatorResolved` (§6.5 above) targets
// exactly {Filled, Cancelled, Rejected, Expired}, never `Reconciled`
// either. `Reconciled` should therefore never actually be observed on a
// live order in this design — but "should never be observed" is exactly
// the class of assumption this whole spec refuses to lean on elsewhere: IF
// a bug, a legacy code path, or log corruption ever DID produce a
// `Reconciled` state for a still-open order, including it in
// is_exchange_final()'s true-set would let compaction discard that order's
// checkpoint and pinned rules snapshot, and release its COID — silently
// abandoning a position that might still be `NEW`/`PARTIALLY_FILLED` on
// the exchange. Excluding it costs nothing for any legitimate code path
// (since none targets it) and fails closed for the illegitimate one.
inline bool is_exchange_final(OrderState s) noexcept {
    return s == OrderState::Rejected || s == OrderState::Filled ||
           s == OrderState::Cancelled || s == OrderState::Expired;
    // Deliberately excludes EscalatedToOperator — is_terminal() includes it,
    // is_exchange_final() does not (still needs operator context retained).
    // Deliberately excludes Reconciled too, per the note above — matches
    // §6.5's authoritative statement exactly; is_terminal() still includes
    // it (unchanged, existing code), is_exchange_final() never does.
    // Every compaction retention rule below uses is_exchange_final(), never
    // is_terminal(), to decide what's safe to drop.
}
```

- **Compaction trigger**: an explicit operator-driven or high-watermark (L4 §10.1) compaction step may rewrite the durable log, retaining:
  1. every non-exchange-final COID (`!is_exchange_final(state)` — includes `EscalatedToOperator`) is rewritten as a leading, self-contained `OrderRecoveryCheckpoint` plus at most `kMaxCheckpointFramesPerOrder - 1` later events. The checkpoint contains immutable intent (`COID`, side/type, intended price/qty, submit rules version), the current resulting state/fill, query/open-poll counters, `last_poll_completed_utc_ms`, and the referenced snapshot pin. It contains **no old-generation sequence number**; counters are values, not references. A raw `OrderIntentCreated`/`OrderSubmitPrepared` pair may be retained in addition for audit, but recovery must not require it.
  2. **uncleared freezes (round-22/25/31/32/33 P0)** — retain per L4 §7.3.1 / `recover_control_plane()`:
     - **Every** uncleared epoch, folding **all** that epoch's live `RateLimitFreeze` frames (and any prior `RateLimitFreezeSnapshot`) into one aggregate (`max(deadline_utc_ms)`, `max(conservative_wait_ms)`, `max(wait_generation)`, worst `source`) — **not** latest-frame-only.
     - **Emit only via L4 `append_compacted_freeze_snapshot`** into `gen-N+1` while `CURRENT` still names N — carries **folded** fields; **must not** call live `append_rate_freeze` (which enforces `prior_folded + 1` on wait-bearing frames and would Reject gen `>1` snapshots or force an illegal bypass). Snapshot write must bind `CompactionFreezeSnapshotProof` (source generation/tip/fold) to `CompactionSourceBaseline`. No `arm_ack_steady` side effects.
     - Permanent (`source==2`) until a **valid** `FreezeClear{OperatorAuthorized}`.
     - Timed uncleared without valid terminal `FreezeClear` (`ProbeVerified` requires `FreezeTimeProbeProof` **and**, when wait>0, a matching sink-verified `FreezeWaitSatisfied` / `CompactedFreezeWaitEvidence` for the **current explicit** `wait_generation > 0`; `ConservativeWaitCompleted` only if `deadline_utc_ms==0` and, when wait>0, the same current-gen WaitSatisfied).
     - Retain current-gen wait evidence **only if** `wait_generation > 0` equals folded gen — emit via L4 `append_compacted_wait_evidence(CompactedFreezeWaitEvidencePayload)` (never live `append_freeze_wait_*`; never dual Arm+Satisfy frames). Drop stale gens **and** all legacy gen `0`/missing pairs that fail path-3.
     - **Legacy-wait seal (round-32/33/34 P0):** if folded wait `> 0` and `max(frame.wait_generation) == 0`, compaction **must** `append_compacted_freeze_snapshot` with explicit `wait_generation = 1` and **drop** legacy Arm/Satisfy — **unless** in the same transaction it applies L4 path-3: cited Arm exists, Satisfy binds Arm, **both** Arm.seq and Satisfy.seq strictly > every wait-bearing live freeze, bound matches; then `append_compacted_wait_evidence` rewritten to gen `1`. Counter-example that must drop: `freeze#1 → Arm → freeze#2 → short-wait Satisfy`. Never silently promote decode-`0` Satisfy to gen `1` without rewrite.
     - Invalid/forged clears are not clears — freeze frames stay retained.
  3. every `SymbolRegistrySnapshot` pinned by a non-exchange-final order (**fixes this round's P0** — was "non-terminal," which would have let an escalated order's snapshot be dropped; see `is_exchange_final()` above), plus the single latest snapshot,
  4. Intent-only orphan frames younger than `kIntentOrphanRetainMs` (§6.2),
   5. the leading `GenerationBridge` frame for the new generation (L4 §10.2), the latest valid `EndpointWeightConfigSet`, and the latest valid `RateLimitUsageSnapshot` needed by `recover_control_plane()`.
   6. every `FreezeProbeAttempt` matching a retained (rule 2) uncleared freeze's `freeze_epoch`; the single latest `FreezeEpochWatermark` **always**; single-frame `CompactedFreezeWaitEvidence` per rule 2 (via `append_compacted_wait_evidence`); terminal clears may be dropped once the epoch is no longer active.
  Snapshots/freezes not in the retain set may be dropped.   **Atomic generation switch with external seal (round-12 P0; tip-before-CURRENT round-20/21 P0; source-tip pin round-34 P0; seal durable inputs round-35…58 P0/P1):**
  0. **Refuse** if `CURRENT` generation is `UINT32_MAX` (L4 §10.1 generation exhaustion). **Refuse** if a prior **`CompactionCandidateIntent`** still exists (finish that candidate’s cleanup first — L4 §10 ABI).
  1. Pin `CompactionSourceBaseline = {N, tip_seq, tip_mac, key_id}`; retain-scan against that tip. **`CREATE_NEW`+fsync packed `CompactionCandidateIntentWire` (140B; phase=Building; baseline 4-tuple + `target_generation=N+1` + `build_nonce`; `candidate_id=request_id=0`; MAC under `kek_key_id`; genesis — no none→Building `.x1`)** **before any `gen-N+1/` byte**; refuse if prior Intent or orphan `CompactionIntentTransitionWire` `.x1` / leftover `CompactionIntentGcAuthorizedWire` `.xgc` remains (L4 §10 ABI / §10.1 / §10.3).
  2. Write `gen-N+1/` contents (`log` + `anchor` + `meta`) including a leading `GenerationBridge` binding `{prev_generation, prev_tip_seq, prev_tip_mac, prev_key_id}` **equal to baseline == Intent.baseline_*** → `{new_generation, new_genesis_seq}` (L4 §10.2 — `prev_key_id` required for tip-MAC key lineage across rotation). Fsync files **and** the `gen-N+1/` directory (L4 §10.3). After every yield resume during this build: if CURRENT tip ≠ baseline → **PreSeal-abandon** N+1 (verify vs Intent; clean G under pre-Started GenGone; TipExportProducerResume if pause armed; **CREATE `.xgc` → GC matching `.x1` → clear Intent → unlink `.xgc` last**), restart from step 1 (no event loss on N). Compacted wait evidence frames must carry `source_baseline_key_id == baseline.key_id`.
  3. After the final retained frame/anchor is fsynced, **re-verify** `CURRENT tip == baseline` **and** `bridge.prev_* == baseline` (seq/mac/`prev_key_id`) **and** Intent baseline. **PreSeal** (no complete MAC-valid `SealExportStarted` yet; Intent still Building|Reserved): mandatory inputs path **A** or **B** under **B-sticky** (L4 §10.1); never volatile; never busy/`Failed`-without-durable. Then PreSeal admission in order (L4 §10.1 / §10.2.1): **(a)** pause tip-export producer, **empty `ExportOutboxRing`**, finish all tip-export RPCs / tip-writer lock; **(b)** network quiesce; **(c)** **refuse** new id reserve until every prior candidate’s **drain complete** — no live `.sj1`, no `.jhw`, no `.jts`, counters consistent, no Started / no `.clr` / no `.abd` / no leftover Intent / no orphan `.x1` / no leftover `.xgc` (L4 §10.1 journal drain + Started clear / ClrAbandoned through ResumeAuthorized + unlink A + TipExportProducerResume + CREATE `.xgc` + GC `.x1` + Intent clear + unlink `.xgc`); then durable-reserve `candidate_id`+`request_id` from **`SealIdWatermark`**, pin journal baseline bind = `CompactionSourceBaseline`, **CREATE_NEW `SealJournalCommitWatermark` (highest=0)** (advance+flush — **before any Path B**), publish **`CompactionIntentTransitionWire` `.x1` seq=1 Building→Reserved** (176B; `HY-COMPINTENT-X-v1`; no-replace; parent flush) → **REPLACE Intent→Reserved** (bind ids); **(d)** in-flight drain under that `candidate_id` — Path A legal only while **zero** journal entries exist; once any journal Acked, Path A illegal (B-sticky); journal admit = packed **SealJournalEntryWire** into preallocated ≤`MaxEntryBytes` → `journal_seq` **only** from on-disk `.jhw` (missing → fail-closed) → exclusive tmp → **no-replace** publish (POSIX `linkat`/`RENAME_NOREPLACE`; Windows **preferred `CreateHardLinkW`+parent `FlushFileBuffers`**, CREATE_NEW copy fallback only; byte-equal if final exists) → load `prev_hw` → monotonic **`.jhw`** (REPLACE allowed for watermark only) → `+=` **both** counters iff created_final ∨ (`prev_hw < journal_seq`) → Ack; gates: size/allowlist/schema/time/baseline/`kek_key_id`/**MUST compute `HY-SEALJRN-v1`**; tip≠baseline ⇒ Path B also illegal; **(e)** soft-cap = free slots (`MaxEntries - journal_entry_count`) **and** owner `journal_bytes_used` over **live `.sj1` only** (tombstones/`.jhw` excluded; no per-admit readdir; `FixedMetaBytes=138` — L4 §10.1); **(f)** **freeze** `SealJournalIntakeCloseControl` topology (`registered_producer_mask` / `producer_count` / `ring_id[]` / `candidate_id`; true-SPSC ⇒ mask=`0b1`) **before** Started; **(g)** require Intent phase==**Reserved** with matching ids/baseline; `CREATE_NEW`+fsync packed **`SealExportStartedWire` v2** onto `seal-export-started` (238B; `HY-SEALSTART-v2` under **`kek_key_id`**; refuse if LegacyStarted/`.v2`/`.mig`/`.clr`/`.abd` present — legacy upgrade is companion+mig only; never try-all KEK; durable `.abd` (AbandonInProgress) must finish ResumeAuthorized + unlink A + TipExportProducerResume before any new Started) reusing reserved ids + **exact** id-reserve baseline pin (**no** live-tip re-sample; **no** second watermark advance); publish `.x1` seq=2 Reserved→StartedPublished → **REPLACE Intent→StartedPublished**; PostSeal begins; **(h)** call **`export_and_wait_ack(GenerationSeal{..., request_id})` directly** — never enqueue seal onto the tip-export ring. Equalities: `GenerationSeal.key_id == bridge.new_key_id ==` N+1 final tip `key_id`; `content_root` matches breadcrumb; Started baseline == `bridge.prev_*` (L4 §10.2). During seal wait, owner-driven poll must journal PostSeal handoff via **true SPSC or per-producer SPSC array** (path B only; multi-writer into one SPSC forbidden) — pure sleep ignoring completions is illegal. Tip producer stays paused until step 5 clears Started **via `.clr` after journal drain-complete**. L4 §10.3 is flush-order only — **not** an alternate PreSeal/cleanup (must match this chain).
  4. **After seal Ack, before `CURRENT` flip (still PostSeal):** re-verify tip == baseline; persist `LastRemoteAckedTip = {store_uuid, new_generation, final_seq, final_tip_mac, key_id}` (monotonic CAS). Tip-persist failure → do not flip; recovery may complete tip+flip only if N tip == `bridge.prev_tip_*` + `prev_key_id`. Mandatory inputs: path **B** into reserved journal only. Tip drift after `SealExportStarted` → `Corrupt`. Discard residual gen-N outbox tuples **without send**; remote must hard-reject lagging `(generation, sequence)`.
  5. Flip `CURRENT` (atomic replace + parent fsync); run L4 **normative PostSeal cleanup chain** (sole path — including any §10.3 flip): **continuity-checked** FIFO `append_seal_journal_apply(SealJournalAppliedView)` → **`SealJournalTombstoneWire`** → unlink `.sj1` → parent flush → decrement **both** counters → **`SealJournalIntakeCloseControl` protocol** (frozen **mask-only** quiesced ACK + admit guard + double-confirm drain + **`close_deadline_steady`** — not bare empty / not wait-all-8) → **post-close apply catch-up** until `applied_hw == commit_hw` → GC A clear `.jhw` → GC B clear `.jts` → journal drain-complete → **PostSealCommittedProof → `.clr` v2 authorize → `.x1` seq=3 StartedPublished→PostSealFinalizing → Intent REPLACE → ordered unlink (M→V→L; presence≫stale phase)** → CAPTURE from C → unlink C → **CREATE `.xgc` (316B DurableCleanupAuthEvidence) → TipExportProducerResume (idempotent) → GC `.x1` → clear Intent → unlink `.xgc` last** (L4 §10.1 / §10.2 / §10.3); close timeout → retain `.jhw` + hard fence + keep Started + **no GC**; GC A while lagging → illegal; hole/damaged/replaced final `.sj1` → `Corrupt` before any apply; **MUST recompute `entry_mac`**; **no** separate `FrameTimeKind` arg; resume tip-export producer for N+1 (PostSeal `.clr` path, after PostSealFinalizing clear); **ClrAbandoned / authenticated-NotFound Started abandon** (with or without C) instead runs A0 `.abd` → … → GenGone → ResumeAuthorized → **`.x1` seq=3 → Intent→AbandonFinalizing (A still present)** → CAPTURE from A → unlink A → **CREATE `.xgc` (316B DurableCleanupAuthEvidence) → TipExportProducerResume (idempotent) for gen-N → GC `.x1` → clear Intent → unlink `.xgc` last** (L4 §10.1); then freeze projection / live appends. During PostSeal, mandatory inputs remain Path B **only while `.jhw` exists**; after successful intake-close + GC A, Path B is fail-closed.
  6. Compaction is chunked/yielded on the owner (L4 §10.1); tip-pin makes yield-safe before PostSeal; after `SealExportStarted` must not yield for unrelated work (journal handoff during seal wait is required work).
  Crash mid-compaction leaves `CURRENT` on the old generation. Old generation is archived read-only only after the new `CURRENT` is durable **and** the external seal ACK exists **and** the tip breadcrumb is at N+1 **and** archived N final tip equals `bridge.{prev_tip_seq, prev_tip_mac, prev_key_id}`. A local CURRENT that jumps backward relative to the newest external seal without a forward bridge chain is `Corrupt` (rollback). Must-pass: prior round-58 cases **plus** `v2 238B + kek_key_id (no try-all)`; `migrate closed L↔V + digests in M`; `PostSealCommittedProof → .clr v2 → Intent PostSealFinalizing → ordered unlink → CAPTURE from C → unlink C → CREATE .xgc (316B) → TipExportProducerResume (idempotent) → GC .x1 → clear I → unlink .xgc`; `§10.3 flip → ordered unlink → crash at .xgc CREATE / each .x1 unlink / Intent unlink / .xgc unlink → Mode B converge when .xgc v2 valid AND Mode-B PhysicalCleanupPreconditions hold from DurableCleanupAuthEvidence (not Corrupt; not “ordered unlink → clear Intent”; early .xgc + leftover Started/C → Corrupt; C/A-gone mid-GC converges)`; `Started+zero-journal+wrongful .clr+Found → query-first`; `last .clr unlink / last Started gone → crash before Intent clear → I.PostSealFinalizing → converge via .xgc-authorized clear (not fence)`; `Started → wrongful C → NotFound → ClrAbandoned (.abd) → Intent AbandonFinalizing → CAPTURE from A → unlink A → CREATE .xgc (316B) → TipExportProducerResume (idempotent) → GC .x1 → clear I → unlink .xgc → new reserve`; `durable .abd → CREATE Started refused until ResumeAuthorized + unlink A`; `pause→Started→NotFound→A0(.abd)→GenGone→ResumeAuthorized→Intent AbandonFinalizing→CAPTURE from A→unlink A→CREATE .xgc (316B)→TipExportProducerResume (idempotent)→GC .x1→clear I→unlink .xgc→try_push` (with-C and no-C); `.abd unlink → crash before Intent clear → I.AbandonFinalizing → converge via .xgc-authorized clear (not fence)`; `Started(no C)→NotFound→clear Started without A → non-compliant`; `G fsync→crash (Building Intent, not yet reserve/Started)→CREATE .xgc→clear I→unlink .xgc+retry (not fence)`; `StartedPublished→Started/A missing→Corrupt/IoError`; `PostSealFinalizing/AbandonFinalizing→Started/A missing + no early .xgc→CREATE .xgc→GC .x1→clear I→unlink .xgc (not Corrupt); early .xgc + leftover gates→Corrupt`; `illegal PostSeal→Abandon / wrong jump + crash → Corrupt (not clear Intent)`; `.x1 durable→crash before Intent REPLACE→complete REPLACE (not false Corrupt)`; `orphan .x1 without .xgc→Corrupt`; `CREATE .xgc → GC .x1 → Intent → .xgc last`; `mid-GC x1 gap with .xgc + gates cleared → converge`; `mid-GC gap without .xgc → Corrupt`; `forged/dual/early .xgc + leftover Started|C|A → Corrupt`; `Intent gone + .xgc + leftover Started → Corrupt (not finish .xgc only)`; `Reserved PreSeal .xgc with terminal_transition_mac=0 while .x1 remain → Corrupt`; `Building PreSeal .xgc with non-zero terminal_transition_mac while no .x1 → Corrupt`; `no Intent+(G|T)→Corrupt`; `Building|Reserved+no Started+no A→PreSeal-abandon (not Corrupt)`; `G+T missing after Started → Corrupt`; `A unlinked before ResumeAuthorized → non-compliant`; `ClrAbandoned leaves gen-N+1/ → non-compliant`; `mid-clear/mid-abandon resumes (never PreSeal/Corrupt/self-lock)`; `migrate L kept + V CREATE_NEW + M bind → MigratedV2`; `power-cut at every migrate cut → never PreSeal / never new ids`; `V without M → LegacyStarted fence`; `delete/replace L → non-compliant` (L4 §10.1 / §10.2 / §10.3).
  **Recovery core validation:** rebuild journal counters from live `.sj1` only; continuity + no-replace disposition for every candidate (hole/MAC-fail/missing middle/replace under `SealJournalCommitWatermark` → `Corrupt`, fence); classify Started/Intent as **AbandonInProgress** / **CleanupInProgress** / **ClrUnauthorized** / **NativeV2Started** / **MigratedV2Started** / **LegacyStarted** / incomplete-mig / torn / **PreSealIntentAbandoned** / **PostSealIntentFinalizing** / **AbandonIntentFinalizing**; verify Started/mig/`.clr`/`.abd`/Intent under wire **`kek_key_id`** (never try-all); **rebuild+freeze** handoff topology from admitted v2 wire (L or V; not live config alone) before demux; `LegacyStarted` / V-without-M → fail-closed companion+mig (single `legacy_kek_key_id`); `Started`+missing `.jhw` → Path B fail-closed (never invent seq); **CleanupInProgress** only if PostSealCommittedProof holds → ensure Intent PostSealFinalizing → resume `.clr` unlink (presence≫stale phase); **ClrUnauthorized** / wrongful `.clr` → query-first (never unlink Started); **AbandonInProgress** / authenticated-NotFound Started abandon (with or without C) → ClrAbandoned (`.abd` A0 first; no-C: present_mask.C=0 + digest_C=0; GenGone → ResumeAuthorized → Intent AbandonFinalizing → CAPTURE from A → unlink A → CREATE `.xgc` (316B) → TipExportProducerResume (idempotent) → GC `.x1` → clear Intent → unlink `.xgc`); verify Intent **`.x1` transition chain** (L4 r66) and optional **`.xgc`** (L4 r67…r72 — live write **316B/`HY-COMPINTENT-GC-v2`/DurableCleanupAuthEvidence**; legacy v1/204B fail-closed) before trusting phase path — Mode A (no Mode-B-eligible `.xgc`): missing/jump/cross/seq gap/prev_mac break/orphan `.x1` → Corrupt; Mode B only when MAC-valid `.xgc` v2 **AND Mode-B PhysicalCleanupPreconditions from DurableCleanupAuthEvidence + live gate absence** (L4 r70/r71 — “valid `.xgc` ⇒ Mode B” WITHDRAWN; never re-read deleted `.clr`/`.abd`): gapped/partial `.x1` → converge GC (NOT Corrupt); Intent gone + Mode-B-eligible `.xgc` + gates cleared → finish `.xgc` (NOT Corrupt); Intent gone + `.xgc` + leftover Started|C|A → Corrupt (MUST NOT “finish unlink `.xgc` only”); PostSealFinalizing + `.xgc` + leftover Started/C → Corrupt; AbandonFinalizing + `.xgc` + leftover A → Corrupt; forged/dual/early `.xgc` / dual disposition mismatch → Corrupt; Reserved/PostSeal/Abandon `.xgc` with `terminal_transition_mac=0` while `.x1` remain → Corrupt; Building PreSeal `.xgc` with non-zero mac while no `.x1` → Corrupt; one-step lag (`.x1` durable, Intent REPLACE pending) → complete REPLACE; no `.x1` with phase>Building without Mode-B-eligible `.xgc` → Corrupt (r66 GC-lag without auth **withdrawn**); Intent Building|Reserved + no Started + no A + no early `.xgc` → PreSeal-abandon (NOT Corrupt; CREATE `.xgc` → GC `.x1` → clear Intent → unlink `.xgc` after G cleanup); Intent **PostSealFinalizing** + no A + **no** `.xgc` → finish TipExportProducerResume + CREATE `.xgc` → GC `.x1` → clear I → unlink `.xgc` (NOT Corrupt even if Started/C gone); Intent **AbandonFinalizing** + **no** `.xgc` → finish unlink A if present + resume + CREATE `.xgc` → GC `.x1` → clear I → unlink `.xgc` (NOT Corrupt even if A gone); Intent StartedPublished + no Started + no A → Corrupt/IoError (non-terminal only); no Intent + no Started + no A + (G|T) → Corrupt/IoError; leftover *Finalizing Intent / residual `.x1` / leftover `.xgc` blocks new build (finish, never overwrite); `NotFound` abandon must not leave unauthorized C or clear Started without A or leave live `gen-N+1/` or paused tip-export producer or use-after-unlink / resume-before-unlink; **resume partial GC** only after re-running intake-close (with deadline) + catch-up when Applied∧no `.sj1`∧no `.jhw`∧ residual `.jts`. Admit N+1 only if `GenerationBridge.prev_key_id` validates `prev_tip_mac` on archived N tip; missing/mismatch → `Corrupt`. If NativeV2/MigratedV2 present → require `SealIdWatermark.next_* > started.ids`; force PostSeal via `query_seal_by_request_id` → **`SealQueryStatus`** (L4 §10.1); `Found` must field-bind breadcrumb + local N+1 **including Started baseline 4-tuple == `bridge.prev_*` (full)**; journal baseline must match Started/bridge; `TransportUnavailable` → keep started (incl. L+V+M), fence, re-query; authenticated **`NotFound` may abandon only if N tip == Started/bridge baseline 4-tuple** (else `Corrupt`); never journal-replay onto N while started exists (except that tip-guarded abandon). Journaled-but-not-Started → drain **all** candidates ascending `candidate_id` through full cleanup chain; tip≠baseline + unapplied → `Corrupt` (B-then-A within one candidate — tip advance from an earlier orphan’s replay is expected). PostSeal drain is the full chain onto N+1; **Started clears only via proof-valid `.clr` v2 after flip+journal drain-complete** (no `.sj1`/`.jhw`/`.jts`; Intent PostSealFinalizing before Started unlink; never unlink L/V/M without PostSealCommittedProof) — never after bare journal replay. Recovery/fold walks allowlisted `SealJournalApplied` embedded payloads as native business frames using the applied frame's own `time_kind`/`recorded_utc_ms` (store frame MAC does not authorize journal apply; complete-but-damaged Acked journal ≠ torn-tail discard; replace-publish forbidden — aligns with §6.1.1 / L4 §10.1).
- **Non-exchange-final pin rule (hard)**: a snapshot referenced by any non-exchange-final order (`!is_exchange_final(state)`, including `EscalatedToOperator`) MUST NOT be deleted by compaction or by any automatic "keep last N versions" heuristic. Violating this makes `recovery_scan()`'s join unable to reconstruct `rules_snapshot_at_submit` → the only honest outcome is `Corrupt`/`CapacityExceeded` and a blocked L5 start — i.e. the self-lock round 8 warned about, reintroduced this round if `is_terminal()` were used here instead of `is_exchange_final()`. The pin rule prevents that class of failure by construction.
- **Live-process guard**: before accepting a refresh, the L5 thread counts distinct pinned `rules_version`s among current in-flight/`OrderRecord`s plus 1 for the prospective new version; if that count would exceed `kMaxTrackedRegistryVersions`, refuse the refresh (same as the compaction-needed path above).

`AuditRingSink` remains as-is for tests and non-L5 paths (dry-run, unit tests); `DurableAuditSink` is a new, separate interface `binance_private_rest.hpp`'s runtime wiring requires — this design does not ask `AuditRingSink` itself to become durable, since that would break its existing test usage.

#### 6.1.4 Per-`clientOrderId` replay is a state-machine walk, not a "read the last event" shortcut — fixes round-7 P1 + round-8 P0

Every description of `recovery_scan()` so far has implicitly assumed the events for a given `client_order_id` form a single, well-ordered, non-contradictory sequence — but nothing has actually defined what happens if the log doesn't: a duplicate event (two `OrderSubmitted` records for the same COID), an out-of-order event (an `OrderAccepted` appearing before any `OrderSubmitted` for that COID), or a contradictory one (an `OrderFilled` appearing after an already-terminal `OrderRejected` for the same COID). Under normal single-writer operation (§6.1.1.1's file-lock discipline) these shouldn't occur — but "shouldn't occur" is exactly the class of assumption this whole design has otherwise refused to make about untrusted, potentially-corrupted durable state.

**Fix**: `recovery_scan()` reconstructs each `client_order_id`'s state by **replaying that COID's events in `sequence_number` order (§6.1.1.1)**, consulting `order_lifecycle.hpp::validate_transition()` only where that function's contract actually applies — not a bespoke "read whatever the last/most-specific-looking event says" heuristic, and not a naive "call `validate_transition` on every event" loop either. **Fixes round-8 P0**: round 7's prose claimed idempotent duplicates were acceptable but its implied algorithm called `validate_transition(replayed_state, record.resulting_state)` on every record and treated any non-`Ok` as `Corrupt`. A direct read of `order_lifecycle.hpp::validate_transition()` shows that function has **no same-state self-loop** (`Intent → Intent`, `Submitting → Submitting` both return `InvalidTransition`) and has no "establish initial Intent" transition at all. Under those actual rules, **the very first event of every normal order** — `OrderIntentCreated` with `resulting_state = Intent` — is `Intent → Intent = InvalidTransition → Corrupt`. A completely normal, uncorrupted log would fail recovery and block startup. The replay must be written against what `validate_transition()` actually does:

```cpp
// Per-COID replay — fixes round-8 P0 (the initial-establish and same-state
// idempotency cases must be handled explicitly; validate_transition() does
// NOT model either of them, so they cannot be expressed as a call to it):
bool established = false;
OrderState replayed_state = OrderState::Intent;  // placeholder until established

// CHANGED — iterates DecodedOrderFrame (defined in §6.1.3 above), not a raw
// AuditRecord: a checkpoint frame and an event frame are two different
// physical record types, and only the decoded, tagged value carries both
// `record_type` and the correct sub-object (`.event` or `.checkpoint`) to
// read from — fixes this round's P0 (record.record_type/.checkpoint had no
// backing type in any earlier revision).
for (each DecodedOrderFrame for this client_order_id, in .sequence order) {
    // (a) FIRST event for this COID must be either OrderIntentCreated
    //     establishing Intent, or OrderRecoveryCheckpoint from a verified
    //     compaction generation. A checkpoint is an ESTABLISH of its embedded
    //     resulting_state and immutable intent; it is not replayed as a raw
    //     transition and its counters replace any old-generation sequence refs.
    //     OrderIntentCreated is also an ESTABLISH, not a transition — do not
    //     feed it to validate_transition() (Intent has no valid predecessor;
    //     there is nothing to transition FROM yet). A first event that is
    //     anything other than "establish Intent" is a genuinely broken /
    //     tampered history for this COID -> Corrupt.
    if (!established) {
        if (record.record_type == DurableRecordType::OrderCheckpoint) {
            if (!checkpoint_is_self_contained_and_pinned(record.checkpoint))
                return RecoveryScanStatus::Corrupt;
            established = true;
            replayed_state = record.checkpoint.recovered.state;
            restore_checkpoint_counters(record.checkpoint);
            continue;
        }
        // record.record_type == OrderEvent from here on — read via .event.
        if (record.event.event_type != AuditEventType::OrderIntentCreated ||
            record.event.resulting_state != OrderState::Intent) {
            return RecoveryScanStatus::Corrupt;
        }
        established = true;
        replayed_state = OrderState::Intent;
        continue;
    }

    // record.record_type == OrderEvent for every iteration below —
    // OrderCheckpoint can only legally appear as the FIRST frame for a COID
    // (compaction always writes it as the establishing frame, never mid-
    // sequence); a checkpoint appearing after `established` is Corrupt.
    if (record.record_type == DurableRecordType::OrderCheckpoint) {
        return RecoveryScanStatus::Corrupt;
    }
    OrderState to = record.event.resulting_state;

    // (b) Same-state re-assertion (X -> X). FIXES ROUND-10 P0: round 9
    //     required the fill payload to match the prior frame EXACTLY — but
    //     §6.6 (L4) polling legitimately observes progressive fills
    //     (PartialFill 0.1 -> PartialFill 0.2 -> ...), each durably recorded
    //     as a fill-progress frame with the SAME resulting_state and a LARGER
    //     cumulative fill. Exact-match replay would judge every normally
    //     progressing partial fill Corrupt on its own recovery. The correct
    //     invariant is MONOTONICITY, not equality — cumulative fills only
    //     grow, never shrink, and never exceed the intended quantity:
    //       - event_type must be compatible with the state (unchanged);
    //       - filled_qty_ticks_new >  replayed_fill.qty  -> legal PROGRESS:
    //           update replayed_fill (qty and avg price) and continue —
    //           bounded by intended_qty_ticks (exceeding it -> Corrupt);
    //       - filled_qty_ticks_new == replayed_fill.qty  -> idempotent
    //           duplicate ONLY if avg_fill_price_ticks also matches
    //           (same fact re-asserted); price mismatch at equal qty
    //           -> Corrupt (two contradictory claims about one fill);
    //       - filled_qty_ticks_new <  replayed_fill.qty  -> Corrupt
    //           (cumulative executedQty can never decrease — a shrinking
    //           fill is tampering or corruption, not progress).
    //     States that carry no fill payload keep the round-9 rule unchanged
    //     (event-type compatibility + payload equality).
    //     NOTE: this is a REPLAY rule for same-state frames; the live path
    //     writes fill progress as a durable frame + in-memory field update
    //     with NO transition_to() call (§4.3 / L4 §6.6) — the state machine
    //     itself gains no PartialFill -> PartialFill self-loop, so
    //     validate_transition() stays untouched by this fix.
    if (to == replayed_state) {
        if (!event_type_compatible_with(record.event.event_type, to) ||
            !fill_payload_monotonic_from(record.event, /*prior=*/replayed_fill,
                                          /*cap=*/intended_qty_ticks)) {
            return RecoveryScanStatus::Corrupt;
        }
        // CHANGED (fixes this round's P0) — was advance_replayed_fill(replayed_fill,
        // record), passing the DecodedOrderFrame itself instead of record.event
        // (the AuditRecord advance_replayed_fill() actually reads fill fields
        // from), inconsistent with the identical call at (c) just below.
        advance_replayed_fill(replayed_fill, record.event);  // no-op for pure duplicates
        continue;
    }

    // (c) A genuine state change: now (and only now) the state machine is the
    //     authority. Anything it rejects is a truly out-of-order or
    //     contradictory sequence for THIS COID -> Corrupt (same fail-closed
    //     outcome §6.1.1 uses for a broken hash chain / bad checksum).
    //     Fill-carrying transitions (e.g. Accepted -> PartialFill,
    //     PartialFill -> Filled) must ALSO satisfy the same monotonic rule as
    //     (b): the new cumulative fill is >= the replayed fill and <= the
    //     intended quantity; Filled additionally requires
    //     filled_qty_ticks == intended_qty_ticks (a "Filled" frame with a
    //     partial quantity is a contradiction -> Corrupt).
    if (validate_transition(replayed_state, to) != TransitionResult::Ok ||
        !fill_payload_monotonic_from(record.event, /*prior=*/replayed_fill,
                                      /*cap=*/intended_qty_ticks)) {
        return RecoveryScanStatus::Corrupt;
    }
    advance_replayed_fill(replayed_fill, record.event);
    replayed_state = to;
}
// (d) An order whose replay never reached `established` (zero events — can't
//     happen for a COID that appears in the log at all, but defensively) is
//     not an in-flight order to recover; it is simply absent.
```

This makes recovery's per-order state reconstruction provably consistent with the exact rules that produced the log — the initial establish, idempotent same-state duplicates, and monotonic fill progress (all three legitimate, none expressible as a `validate_transition()` call) are handled explicitly, and only a genuine contradiction — an illegal state change, a shrinking or over-cap fill, or a price that changes at unchanged quantity — is treated as evidence of corruption, rather than a normal log (including a normally progressing partial fill) being wrongly rejected.

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
if (!ctx.durable_audit->append_durable(intent_record).acked()) {  // .acked() —
    // this round's AuditAppendResult struct change (§10); every append_durable()
    // check in both spec documents reads this way now
    result.gate = OrchestratorGate::AuditWriteNotAcked;  // new gate value, §4.1
    return result;  // fail-closed — no further processing, no POST
}

// ... CONFIRM check, port-validity check, InFlightRegistry registration
//     (§3 Gate 6) — OrderRecord::state stays Intent through here and through
//     Gates 7-8 (reservation, sign/format); a failure at either releases the
//     InFlightRegistry slot with no state to unwind, fixes this round's P0 ...

// Reservation and exact formatting/signing have already succeeded (§3 Gates 7–8).
// This frame is intentionally named Prepared: it is the durable uncertainty
// boundary immediately before the first transport write, not evidence a POST
// completed.
AuditRecord prepared_record = /* OrderSubmitPrepared, resulting_state=Submitting */;
if (!ctx.durable_audit->append_durable(prepared_record).acked()) {
    // A Failed append may have completed physically after its flush deadline.
    // Never release InFlightRegistry's slot here: doing so would let recovery
    // and live state disagree about an observable durable Prepared frame. The
    // writer is fenced; stop L5 and let a healthy next process recover
    // Prepared as Ambiguous and reconcile conservatively. OrderRecord::state
    // is NOT transitioned to Submitting on this path (fixes this round's P0
    // — the transition only happens on the Acked path immediately below);
    // it remains Intent, consistent with every other pre-Prepared failure.
    result.gate = OrchestratorGate::AuditWriteNotAcked;
    fence_and_stop_l5(ctx, "prepared audit write not acked");
    return result;
}
ctx.order_record->transition_to(OrderState::Submitting);  // NEW — fixes this
    // round's P0: the sole place Intent -> Submitting happens, immediately
    // after Prepared is genuinely Acked (§3 Gate 9), not earlier at Gate 6.

// Only here may the first transport write of submit_order() begin. A crash
// before or during that write is intentionally indistinguishable on recovery
// and is reconciled; no path claims it was definitely never sent.
```

**Intent-only orphans (P1, round 9):** `OrderIntentCreated` is ACKed before CONFIRM/port/in-flight. If the process stops after Intent and before `OrderSubmitPrepared`, recovery sees an established `Intent` with no later event. Policy: Intent-only COIDs are **not** entered into `InFlightRegistry`, are **not** reconciled, and do **not** count toward `kMaxInFlight`. They are retained in the durable log for audit, counted against a separate `kMaxIntentOrphans` budget during recovery (`CapacityExceeded` if exceeded — forces compaction/operator cleanup), and may be archived by compaction once older than `kIntentOrphanRetainMs`. They must never be mistaken for in-flight orders.

This is the concrete difference from revision 2: the durability check is a real gate with a real failure path (`AuditWriteNotAcked`), evaluated with the actual `append_durable()` return value inspected — not an assumption that calling `append()` (fire-and-forget, as all 16 existing call sites do today) constitutes "confirmed."

### 6.3 Why this can't wait

A crash between "POST sent" and "outcome recorded" is exactly the ambiguous-order scenario this whole gate chain exists to handle safely — and it's unrecoverable if the *intent itself* wasn't durably recorded first. Deferring persistent audit "for later" means the highest-risk moment (an in-flight order during a crash) is the one moment this design can't account for. §6.1's `recovery_scan()` is the other half of this: a crash-and-restart must reconstruct in-flight state from the durable log, not start `InFlightRegistry` empty and risk a duplicate submission of an order that was actually still pending when the process died.

### 6.4 The response outcome itself must be durably ACKed before any state transition or in-flight release — fixes round-6 P0

§6.2 covers the two writes *before* the network call (`OrderIntentCreated`, `OrderSubmitPrepared`). Nothing so far covers the write *after* — once the HTTP response actually comes back and §4.3's routing table picks an outcome (`Accepted`, `Rejected`, `Filled`, `PartialFill`, or `Ambiguous`), the spec as written simply transitions `OrderRecord::state` and releases `InFlightRegistry` (for terminal outcomes) with no audit write in between. A crash in that window has a real, specific consequence: the durable log still ends at `OrderSubmitPrepared` (the last thing actually ACKed), so `recovery_scan()` correctly treats the order as still needing reconciliation — but if `InFlightRegistry` had *already* been released in memory before the crash (for a `Rejected` outcome, say), and the release itself was never durably recorded, recovery has no way to know a release ever happened, and — more importantly — the *actual* resolved state (`Rejected`, with its reason) is lost entirely; reconciliation has to rediscover it from Binance from scratch, and if Binance's own side never persisted the rejected attempt (a `-2010`-class case), reconciliation gets repeated `-2013`/`Inconclusive` and escalates to an operator for an order that was, in fact, already cleanly resolved.

**Fix**: every outcome-driven state transition durably ACKs the resulting state *before* it's treated as final — mirroring §6.2's existing discipline for the pre-send writes, not a new pattern:

```cpp
// After §4.3 selects an outcome and the corresponding OrderState:
AuditRecord result_record = /* event_type mapped from outcome (OrderAccepted/
    OrderRejected/OrderFilled/OrderPartialFill/OrderAmbiguous), resulting_state
    set to the new OrderState (§6.1.2's new field), side/order_type/rules_version
    carried from ctx.pre_trade_rules_snapshot; for a fill, price_ticks/qty_ticks
    are reused to carry filled_qty_ticks/avg_fill_price_ticks — same two fields,
    meaning determined by event_type, consistent with how this struct is already
    used generically across different event types today */;

if (!ctx.durable_audit->append_durable(result_record).acked()) {
    // FIXES ROUND-11/12 P0 (self-contradiction): earlier revisions transitioned
    // to Ambiguous here and promised L4 §6.2 reconciliation would rediscover
    // the outcome — but §6.1.1.1's fence contract says ANY append_* Failed
    // permanently fences the writer for this process lifetime, and
    // reconciliation GETs require a durable attempt record before the GET.
    // Those two paths cannot both execute: either the sink is fenced (no
    // durable attempts → no GETs) or reconciliation continues (requires
    // unfenced appends). The fence wins — it is the stronger, more recent
    // durability invariant.
    //
    // Correct handling:
    // 1. Do NOT transition to the (possibly terminal) outcome state.
    // 2. Do NOT release InFlightRegistry.
    // 3. Do NOT transition to Ambiguous in the hope of reconciling in THIS
    //    process — that path is unreachable under a fenced sink.
    // 4. Leave the in-memory state at Submitting (last durably-ACKed fact).
    // 5. The Failed append has already fenced the writer (§6.1.1.1) — L5
    //    stops: no new submits, no reconciliation GETs, no open-order polls
    //    that require durable records.
    // 6. Escalate out-of-band immediately so an operator can repair storage
    //    and restart. On the NEXT process start, recovery_scan() remaps
    //    Submitting → Ambiguous (§6.1) and reconciliation resumes against a
    //    healthy sink — the same recovery a crash-at-this-point would take.
    //
    // Do NOT attempt a best-effort second append_durable(OrderAmbiguous):
    // the sink is fenced; that call returns Failed immediately and adds
    // nothing. The round-7 "leave Ambiguous so determine_reconcile_action
    // can fire" motive is preserved across the restart boundary, not inside
    // a fenced process that cannot durably record attempt counts.
    escalate_via_out_of_band_channel(ctx,
        "post-response outcome audit write not acked — writer fenced, L5 stopped");
    return;  // fenced; no further automated I/O for any order
}

// Only now: rec.transition_to(new_state); release InFlightRegistry only if
// is_exchange_final(new_state) (§6.5). Both are durably-confirmed.
```

This closes the post-response durable-ACK gap without inventing a reconciliation path that the fence contract makes impossible. The round-7 concern ("Submitting is unreachable by `determine_reconcile_action`") is answered by recovery-time remapping on the next healthy start, not by pretending this fenced process can still reconcile.

### 6.5 `EscalatedToOperator` lifecycle — fixes round-10 P1 (escalations permanently exhausted in-flight capacity)

`EscalatedToOperator` may remain terminal for legacy generic state cleanup, but it is **not exchange-final**: it may genuinely be live on the exchange. The implementation must add `is_exchange_final(OrderState)` and use it, rather than `is_terminal()`, at every COID-release, reconciliation/poll, recovery-output, and ledger-release call site. `is_exchange_final` returns true only for `Filled`, `Cancelled`, `Rejected`, and `Expired`; it returns false for `EscalatedToOperator`. This explicit split prevents the existing `is_terminal(EscalatedToOperator)` branch from silently releasing its COID or bypassing `EscalatedLedger`. Escalated orders otherwise sat in `InFlightRegistry` forever, so 64 escalations — reachable during any sustained exchange incident — would permanently zero submission capacity with no recovery short of a process restart that would *also* faithfully restore all 64. Fixed with an explicit two-container lifecycle:

1. **Move on durable ACK**: when the `OrderEscalated` event is durably `Acked` (L4 §6.2 / §6.6's escalation sites), the order moves from `InFlightRegistry` to a fixed-capacity **`EscalatedLedger`** (`kMaxEscalated` from §6.1 — separate from `order_lifecycle.hpp`'s `kMaxInFlight`, zero heap). The in-flight slot is freed — new *intents* (which always carry new COIDs) can submit again. If the append is not `Acked`, no move happens (the sink is fencing anyway).
2. **COID-reuse still blocked**: the duplicate-submission guard consults *both* containers — a COID present in either is refused. Freeing capacity must not quietly re-legalize the one COID whose outcome a human is still deciding.
3. **No automated I/O for ledger entries**: reconciliation and open-order polling both skip `EscalatedToOperator` (existing `determine_reconcile_action()`/`determine_open_order_poll()` behavior — this section changes *capacity accounting*, not retry semantics). The system stopped because it ran out of safe automated moves; only a human adds information now.
4. **Operator resolution is a durable event**: the operator resolves an entry via an explicit command that appends `AuditEventType::OrderOperatorResolved` (new value) carrying the operator-confirmed terminal state (`Filled`/`Cancelled`/`Rejected`/`Expired`) and, when applicable, final fill fields (monotonic rule §6.1.4 applies). Only a durable `Acked` of that event releases the ledger entry. `EscalatedToOperator → {Filled, Cancelled, Rejected, Expired}` transitions are added to `validate_transition()`'s proposed changes (operator-resolution edges; replay-legal for exactly this event type).
5. **Recovery**: `recovery_scan()` (§6.1 ABI — both arrays required) rebuilds the ledger the same way it rebuilds the registry — an escalated order whose log ends without `OrderOperatorResolved` recovers into `out_escalated`, not `out_inflight`; a resolved one recovers as its terminal state (absent from both). Overflow of *either* array is `CapacityExceeded`.
6. **Overflow fails closed, with an early alarm**: at `kEscalatedHighWatermark = 32` the out-of-band operator channel gets an aggregate alert (something systemic is wrong — escalations should be rare). A 65th escalation with a full ledger blocks **new order submission** entirely (Gate 6 refuses) until the operator resolves entries — 64 unresolved human-decision orders is not a state in which placing more orders is defensible; the escalated orders themselves are never dropped to make room.

`SubmitPort` capacity math changes accordingly: worst-case tracked orders = `kMaxInFlight + kMaxEscalated` (128), both fixed arrays, both bounded at compile time.

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
7. **Mandatory fault-injection matrix (rounds 8–73)** — must pass before any claim that P0s are closed in an implementation (current `native/` is pre-revision; this prose alone never closes implementation-level findings). Round-73 must-pass set includes prior round-29–72 cases plus:
   - **Early `.xgc` / Mode B DurableCleanupAuthEvidence (round-70/71/72/73 P0)** → before Mode B converge, verify Mode-B PhysicalCleanupPreconditions from **`.xgc` DurableCleanupAuthEvidence** + live gate absence (never re-read deleted `.clr`/`.abd`; TipExportProducerResume idempotent after auth, not “already executed” durable fact): PreSealAbandonClear (evidence zeros + G/T/journal cleanup); PostSealFinalizingClear (`.xgc` post_seal_committed_bound + journal_drain + seal identity + **L/V/M/C/Started absent**); AbandonFinalizingClear (`.xgc` gen_gone + resume_authorized + **A/Started/C absent**); `intent_phase_at_auth=PostSealFinalizing` is **not** cleanup-complete proof; `PostSealFinalizing + early .xgc + leftover Started/C → Corrupt (MUST NOT Mode B)`; `AbandonFinalizing + early .xgc + leftover A → Corrupt`; `Intent gone + .xgc + leftover Started → Corrupt (not finish .xgc only)`; `.xgc` + leftover C only → Corrupt; dual disposition mismatch / forged evidence / legacy v1 204B → Corrupt; must-pass `C deleted → .xgc written → delete any x1 → crash` and `A deleted → .xgc written → delete any x1 → crash` → Mode B without live C/A; mid-GC after gates cleared still Mode B converges; Forbidden as prescribed write: Clear(r67) `204B`/`HY-COMPINTENT-GC-v1` or TipExport-before-CREATE / `unlink A → resume → CREATE` (L4 r72 / §10 ABI / §10.1 / §6.1.3).
   - **§10.3 unified chain `.xgc` tail (round-69…73 P0)** → every `CURRENT` flip / §6.1.3 step 5 PostSeal clear = `… → Intent PostSealFinalizing → ordered unlink M→V→L → CAPTURE from C → unlink C → CREATE .xgc (316B DurableCleanupAuthEvidence) → TipExportProducerResume (idempotent) → GC .x1 → clear Intent → unlink .xgc last`; Abandon sibling = `… → AbandonFinalizing → CAPTURE from A → unlink A → CREATE .xgc → TipExportProducerResume (idempotent) → GC .x1 → clear Intent → unlink .xgc last`; Forbidden as complete terminal path: `ordered unlink → clear Intent`, `resume → clear I` without `.xgc`; must-pass: `§10.3 flip → ordered unlink → crash at .xgc CREATE / each .x1 unlink / Intent unlink / .xgc unlink → Mode B converge when .xgc v2 valid AND Mode-B PhysicalCleanupPreconditions hold from durable evidence (not Corrupt; early .xgc + leftover Started/C → Corrupt; no live C/A required)` (L4 §10.3 / §10.1 / §6.1.3).
   - **CompactionIntentGcAuthorized mid-GC + early-`.xgc` fence (round-67…73 P0)** → terminal clear: durable no-replace `.xgc` (316B; `HY-COMPINTENT-GC-v2`; filename `compaction-intent-gc-<build_nonce_hex16>`; DurableCleanupAuthEvidence) **before** any `.x1` unlink **only when CREATE-time PhysicalCleanupPreconditions hold**; order CAPTURE→unlink C/A → CREATE `.xgc` → TipExportProducerResume (idempotent) → unlink `.x1` one-by-one (parent flush) → unlink Intent → unlink `.xgc` last; `mid-GC delete x1[1] from {1,2,3} with I+.xgc + gates cleared → restart converges (NOT false Corrupt)`; `same gap without .xgc → Corrupt`; `power-cut before/after each seq unlink / Intent unlink / .xgc unlink → converge under Mode-B-eligible .xgc or Corrupt only when unauthorized/early`; `Intent gone + Mode-B-eligible .xgc (+ residual .x1) → finish clear .xgc (NOT Corrupt)`; `Intent gone + .xgc + leftover Started|C|A → Corrupt`; `forged/wrong-bind/dual/early .xgc / dual disposition mismatch → Corrupt`; `CREATE .xgc before terminal phase / while gates open / wrong disposition → Corrupt`; `Reserved/PostSeal/Abandon .xgc with terminal_transition_mac=0 while .x1 remain → Corrupt`; `Building PreSeal .xgc with non-zero terminal_transition_mac while no .x1 → Corrupt`; phase>Building + no `.x1` + no Mode-B-eligible `.xgc` → Corrupt (r66 GC-lag without auth **withdrawn**); every Recovery/crash-window/§10.3 finish path MUST CREATE `.xgc` before clear I; §10.3 must not reintroduce `ordered unlink → clear Intent` shorthand; “valid `.xgc` ⇒ Mode B” WITHDRAWN (L4 §10 ABI / §10.1 / §10.3 / §6.1.3).
   - **CompactionIntentTransitionWire path proof (round-66/67)** → every Intent phase raise: durable no-replace `.x1` (176B; `HY-COMPINTENT-X-v1`; filename `(build_nonce, transition_seq)`) **before** Intent REPLACE; genesis = CREATE Building (no none→Building `.x1`); recovery Mode A verifies contiguous legal chain / prev_mac; `illegal PostSealFinalizing→AbandonFinalizing (or wrong jump) + crash → Corrupt (not clear Intent)`; `.x1 durable → crash before Intent REPLACE → recovery completes REPLACE / resumes raise (not false Corrupt / not accept cross)`; orphan `.x1` without Intent and without Mode-B-eligible `.xgc` → Corrupt; PreSeal-abandon / final clear: CREATE `.xgc` → GC `.x1` → Intent → `.xgc` last; Intent.phase ahead of last `to_phase` without `.x1` (Mode A) → Corrupt; Intent phase monotonic REPLACE alone does **not** prove no-cross (L4 §10 ABI / §10.1 / §6.1.3).
   - **Terminal Intent Finalizing (round-65…73)** → PostSeal: `.clr` Authorized → `.x1` → Intent **PostSealFinalizing** before any Started unlink → ordered unlink M→V→L → CAPTURE from C → unlink C → CREATE `.xgc` (316B evidence) → TipExportProducerResume (idempotent) → GC `.x1` → clear Intent → unlink `.xgc`; Abandon: ResumeAuthorized → `.x1` → Intent **AbandonFinalizing** while A present → CAPTURE from A → unlink A → CREATE `.xgc` → TipExportProducerResume (idempotent) → GC `.x1` → clear Intent → unlink `.xgc`; `last .clr unlink / last Started gone → crash before Intent clear → I.PostSealFinalizing → restart converges via .xgc-authorized clear (not fence)`; `.abd unlink → crash before Intent clear → I.AbandonFinalizing → restart converges via .xgc-authorized clear (not fence)`; `PostSealFinalizing + leftover C/M/V/L + no .xgc → resume CleanupInProgress → CAPTURE→unlink C→CREATE .xgc → TipExportProducerResume (idempotent) → GC .x1 → clear I → unlink .xgc`; `PostSealFinalizing + .xgc + leftover C/M/V/L → Corrupt (early .xgc)`; `AbandonFinalizing + A absent + no .xgc → resume → CREATE .xgc → GC .x1 → clear I → unlink .xgc`; `AbandonFinalizing + .xgc + leftover A → Corrupt`; `StartedPublished + no Started + no A → Corrupt` (non-terminal only); leftover *Finalizing / residual `.xgc` blocks new build (never overwrite); phase PostSeal↔Abandon cross → Corrupt (via `.x1` chain, not Intent.phase alone); Intent wire stays **140B** / format_version=1; no `completion_kind`; transition wire **176B**; GC auth wire **316B** (`HY-COMPINTENT-GC-v2` + DurableCleanupAuthEvidence) + `static_assert`; legacy v1/204B fail-closed (L4 §10 ABI / §10.1 / §6.1.3).
   - **CompactionCandidateIntent PreSeal vs Corrupt (round-64…71)** → `Intent Building CREATE before any gen-N+1/ byte` (genesis; no none→Building `.x1`); `G fsync → crash (not yet reserve/Started) → I.Building → CREATE .xgc (zeros terminal mac) → clear I → unlink .xgc + retry (not fence)`; `Reserved → crash before Started → replay-to-N / PreSeal-abandon, ids not reused; CREATE .xgc + GC .x1 → clear I → unlink .xgc`; `non-terminal StartedPublished → Started/A abnormally missing → Corrupt/IoError` (Finalizing ≠ Corrupt); `Building|Reserved + no Started + no A → PreSeal-abandon (not Corrupt)`; `no Intent + (G|T) → Corrupt`; leftover Intent (incl. *Finalizing) / orphan `.x1` / leftover `.xgc` blocks new build; phase regression / Mode-A `.x1` chain break → Corrupt; Intent clear only after final cleanup + `.xgc`-authorized `.x1` GC (`.clr`+PostSealFinalizing / ClrAbandoned A4+AbandonFinalizing / PreSeal-abandon) (L4 §10 ABI / §10.1 / §6.1.3).
   - **NotFound-without-C requires `.abd` A0 (round-63…71)** → `Started(no C) → authenticated NotFound → CREATE .abd (mask.C=0,digest_C=0) before any Started unlink → A1 no-op → A2…GenGone→ResumeAuthorized→Intent AbandonFinalizing→CAPTURE from A→unlink A→CREATE .xgc (316B)→TipExportProducerResume (idempotent)→GC .x1→clear I→unlink .xgc`; `clear Started without A → non-compliant`; `non-terminal StartedPublished + no Started + no A → Corrupt/IoError (not PreSeal; AbandonFinalizing ≠ Corrupt)`; power-cut at every A0…A4 / `.xgc` GC step; no new id reserve mid-abandon; no generation overwrite; producer must not permanently stall; Path-A PreSeal-abandon still does **not** require `.abd`; `TransportUnavailable → never .abd` (L4 §10 ABI / §10.1 / §6.1.3).
   - **ResumeAuthorized before unlink `.abd` (round-62…72)** → `ResumeAuthorized flush while A present (producer paused) → Intent AbandonFinalizing → CAPTURE from A → unlink A → CREATE .xgc → TipExportProducerResume (idempotent) → GC .x1 → clear I → unlink .xgc`; `A3 unlink then A3b read A → non-compliant use-after-unlink`; `resume-before-unlink → tip-advance self-lock → non-compliant`; recovery: ResumeAuthorized ⇒ tip==baseline from A → AbandonFinalizing → CAPTURE → unlink → CREATE `.xgc` → resume (idempotent) → GC `.x1` → clear I → unlink `.xgc` (L4 §10 ABI / §6.1.3).
   - **GenGone no-replace + no false idempotency (round-62)** → `T CREATE no-replace; T exists wrong bind → Corrupt`; `G+T both missing after admitted Started → Corrupt/IoError`; `rename→power-cut before parent flush → recovery bind-verifies T`; `G absent without T legal only PreSeal-abandon before Started` (L4 §10 ABI / §6.1.3).
   - **ClrAbandoned GenGone + TipExportProducerResume (round-61…73)** → `pause → Started → authenticated NotFound → A0 (.abd) → GenGone → ResumeAuthorized → Intent AbandonFinalizing → CAPTURE from A → unlink A → CREATE .xgc (316B) → TipExportProducerResume (idempotent) → GC .x1 → clear I → unlink .xgc → try_push → remote tip advances` (with-C and no-C); `ClrAbandoned leaves live gen-N+1/ → non-compliant`; `NotFound abandon without producer resume → non-compliant hard-lag`; Forbidden: `unlink A → resume → CREATE` / TipExport-before-CREATE (L4 §10.1 / §10.2.1 / §6.1.3).
   - **Started greenfield refuses residual `.abd` (round-60…63)** → `(g)` refuse if LegacyStarted/`.v2`/`.mig`/`.clr`/`.abd`; `durable .abd + watermark reserved → CREATE Started refused until ResumeAuthorized + unlink A` (L4 §10.2 / §6.1.3).
   - **ClrAbandoned (round-59…71)** → `ClrUnauthorized + authenticated NotFound + tip==baseline + empty journal → CREATE .abd → unlink C→Started→GenGone→ResumeAuthorized→Intent AbandonFinalizing→CAPTURE from A→unlink A→CREATE .xgc (316B)→TipExportProducerResume (idempotent)→GC .x1→clear I→unlink .xgc → new candidate reserve`; same for no-C with mask.C=0 + digest_C=0; `NotFound abandon leaving unauthorized C → non-compliant self-lock`; `TransportUnavailable → never .abd`; ids never reused (L4 §10 ABI / §6.1.3).
   - **`.clr` PostSealCommittedProof (round-58/65)** → CREATE/CleanupInProgress only when `CURRENT==new_generation` ∧ `LastRemoteAckedTip` equal-or-forward covers Started new_* ∧ bridge binds baseline/ids/content_root ∧ journal drained; after Authorized → `.x1` → Intent PostSealFinalizing before Started unlink; `Started + zero journal + wrongful .clr + remote Found → query-first, Started kept, no generation fork`; draft 176B `.clr` → never CleanupInProgress (L4 §10 ABI / §6.1.3).
   - **Started kek_key_id + `.clr` clear (round-57)** → v2 238B MAC under wire `kek_key_id` (no try-all/current-key); mig v2 208B binds digests+both kek ids; closed L↔V equalities; proof-valid `.clr` → ordered unlink; power-cut mid-clear resumes (presence≫stale phase; never PreSeal/Corrupt); early `.clr` with live journal → Corrupt; `.clr` against LegacyStarted / kind mismatch → Corrupt (never erase sole gate); draft 234B Started / 136B mig → Corrupt (L4 §10 ABI / §6.1.3).
   - **Started companion migrate (round-56)** → keep legacy L; `CREATE_NEW` V + M bind; `L+V+M → MigratedV2`; `V without M → LegacyStarted fence`; `delete/replace L → non-compliant`; power-cut at `L / V.tmp / V / M.tmp / M` → never PreSeal / never new ids (L4 §10 ABI / §6.1.3).
   - **§10.3 single chain (round-55…73)** → generation-directory steps 4/6 **delegate** to §10.1/§10.2; `§10.3 abbreviated apply→close→GC → non-compliant`; `§10.3 abbreviated ordered unlink → clear Intent (no TipExportProducerResume / .xgc) → non-compliant`; every `CURRENT` flip = `freeze → Started(v2+kek) → … → tip → flip → apply → close → catch-up → GC A/B → journal drain → PostSealCommittedProof → .clr authorize → .x1 → Intent PostSealFinalizing → ordered unlink M→V→L → CAPTURE from C → unlink C → CREATE .xgc (316B) → TipExportProducerResume (idempotent) → GC .x1 → clear Intent → unlink .xgc last` (L4 §10.3 / §6.1.3).
   - **SealExportStartedWire v2 (round-55…57)** → packed 238B with `kek_key_id`; `format_version`/`total_bytes` in MAC; `legacy 192B Started → fence, not empty-topology PostSeal`; companion+mig only (L4 §10 ABI / §6.1.3).
   - **Post-close apply catch-up (round-54/55)** → after intake-close SUCCESS, FIFO apply until `applied_hw == commit_hw` before GC A; `close drain → new .sj1 → GC A without catch-up → illegal` (L4 §10.1 / §6.1.3).
   - **Durable Started topology (round-54/55)** → v2 wire MAC-binds mask/count/`ring_id[]`; recovery freeze **must equal** Started; config-drift mismatch → fence before demux (L4 §10.1 / §6.1.3).
   - **Registered producer mask (round-53…55)** → freeze mask/count/`ring_id[]` before PostSeal; close waits **mask-only**; `true-SPSC mask=1 → does not wait slots 1..7`; unset-bit admit → immediate fence; timeout retry bumps epoch (L4 §10.1 / §6.1.3).
   - **Bounded intake-close (round-53…55)** → `close_deadline_steady` + max poll + `_mm_pause`; `hung producer → timeout → retain .jhw + hard fence + Started kept + no GC` (L4 §10.1 / §6.1.3).
   - **Clear Started only via proof-valid `.clr` or ClrAbandoned `.abd` (round-52…73)** → PostSeal path: tip→flip→apply→tombstone→intake-close→catch-up→GC A/B → PostSealCommittedProof → `.clr` authorize → `.x1` → Intent PostSealFinalizing → ordered unlink → CAPTURE from C → unlink C → CREATE `.xgc` (316B) → TipExportProducerResume (idempotent) → GC `.x1` → clear Intent → unlink `.xgc`; authenticated-NotFound Started abandon (with or without C): `.abd` A0 first then C (if any)→Started→GenGone→ResumeAuthorized→`.x1`→Intent AbandonFinalizing→CAPTURE from A→unlink A→CREATE `.xgc`→TipExportProducerResume (idempotent)→GC `.x1`→clear Intent→unlink `.xgc`; `Tip@N+1 → clear Started after bare journal replay / without .clr → non-compliant`; `NotFound abandon leaving C / clearing Started without A / live gen-N+1 / paused tip-export → non-compliant`; power-cut mid-clear/mid-abandon / after names gone before Intent clear / mid-`.x1` GC under Mode-B-eligible `.xgc` / §10.3 flip crash at `.xgc` CREATE or each `.x1`/Intent/`.xgc` unlink resumes via Finalizing+Mode B when PhysicalCleanupPreconditions hold; early `.xgc` + leftover gates → Corrupt (L4 §10.1 / §10.2 / §10.3 / §6.1.3).
   - **Linearizable intake-close (round-52…55)** → close epoch + admit guard + mask-only quiesced ACK + double-confirm drain; `late SPSC tail after naive empty → must not land after .jhw delete` (L4 §10.1 / §6.1.3).
   - **Intake-closed before GC A (round-51…55)** → protocol success + catch-up before delete `.jhw`; `last Applied → GC A + concurrent PostSeal handoff → .jhw retained or close success`; `Started`+missing `.jhw` → Path B fail-closed, never invent seq (L4 §10.1 / §6.1.3).
   - **Windows journal IO + crash-window table (round-50)** → preferred `CreateHardLinkW`+parent `FlushFileBuffers`; CREATE_NEW copy fallback only; `-=` only after unlink parent flush; preallocated ≤`MaxEntryBytes`; every cut `.tmp→…→intake-close→catch-up→GC A/B`; power-cut honesty = §10.3 (L4 §10.1 / §10.3 / §6.1.3).
   - **Drain-complete / counter idempotency (round-49)** → end state = no `.sj1`/`.jhw`/`.jts`; `+=` both counters iff created_final ∨ (`prev_hw < seq`); `crash after .jhw delete with residual .jts → resume GC → new candidate admits`; blind “skip += on every byte-equal” → non-compliant (L4 §10.1 / §6.1.3).
   - **No-replace `.sj1` publish (round-48)** → `renameat2(RENAME_NOREPLACE)` / Windows hardlink preferred; `Acked final → republish same seq different payload → Corrupt, original unchanged`; `publish → HW → crash before Ack → restart same seq → byte-equal Ack, zero second write`; §10.3 REPLACE forbidden for finals (L4 §10.1 / §6.1.3).
   - **Tombstone lifecycle / GC (round-48/49)** → `SealJournalTombstoneWire` (`kSealJournalTombstoneBytes=108`) binds `entry_mac`; order Applied→`.jts`→unlink `.sj1`→parent flush→both counters; GC phase A then B + partial resume; soft-cap = live `.sj1` only; `apply+tombstone+delete all → counters 0 → new candidate admits`; refuse new reserve until drain complete (L4 §10.1 / §6.1.3).
   - **Journal continuity / Acked-middle (round-47)** → `SealJournalCommitWatermark` (monotonic); continuity `1..commit_hw`; `Acked seq=1,2 → delete/truncate seq=2 → restart → Corrupt, zero replay, zero CURRENT flip`; final `.sj1` length/MAC/filename anomaly with `seq≤commit_hw` → `Corrupt`; only `*.sj1.tmp` may discard; complete-but-damaged Acked journal ≠ store-log torn-tail discard (L4 §10.1 / §6.1.1 / §6.1.3).
   - **entry_mac MUST-recompute (round-46)** → every admit / recovery-load / `append_seal_journal_apply` recomputes `HY-SEALJRN-v1` under CURRENT `store_uuid` + `kek_key_id`; `old entry_mac + tampered payload/provenance/baseline → Corrupt, zero business effects` (first-apply and idempotent); opaque-only compare / store-frame-MAC substitute → non-compliant (L4 §10.1 / §6.1.3).
   - **SealJournalEntryWire layout (round-46/47)** → packed LE FixedMeta=`138`; framing in MAC domain; soft-cap = slots **and** live-`.sj1` owner counters (no per-admit readdir); `static_assert`/encode-length tests; guessed `128` → non-compliant (L4 §10.1 / §6.1.3).
   - **B-sticky PreSeal (round-45)** → Path A illegal after any journal Ack for `candidate_id`; `Path B freeze → Path A freeze → illegal/Corrupt`; `unapplied journals + tip≠baseline → Corrupt`; store observation order preserved on abandon replay (L4 §10.1 / §6.1.3).
   - **Started/Found baseline copy (round-45)** → `SealExportStarted` copies exact id-reserve pin (== `bridge.prev_*`); no live-tip re-sample; `Found` requires full baseline 4-tuple bind before tip/flip/journal; committed marker == `entry_mac` under `HY-SEALJRN-v1` (L4 §10.1 / §6.1.3).
   - **NotFound tip guard (round-45)** → authenticated `NotFound` may PreSeal-abandon **only if** N tip == Started/bridge baseline 4-tuple; else `Corrupt` (L4 §10.1 / §6.1.3) — closes prior L5 “NotFound alone may abandon” fail-open.
   - **Journal baseline tip bind (round-44)** → `{source_generation, baseline_tip_seq, baseline_tip_mac, baseline_key_id}` in journal entry + `SealJournalAppliedView` + `entry_mac`; PostSeal/`Found` require bind == `SealExportStarted`/bridge; MAC-omitted baseline → non-compliant (L4 §10.1 / §6.1.3).
   - **Seal-journal time provenance (round-43)** → journal entry + `SealJournalAppliedView` carry `time_kind`+`recorded_utc_ms` in `entry_mac` domain; apply/replay verbatim; no separate `FrameTimeKind` on `append_seal_journal_apply`; `UnknownBootstrap journal → crash → clock published → replay still Unknown+0`; `UTC journal → clock loss → replay still original recorded_utc_ms`; hard-lag age uses preserved UTC (L4 §10.1 / §6.1.1 / §6.1.3).
   - **Id-before-Path-B (round-42)** → durable watermark reserve of `candidate_id`+`request_id` before any seal journal; Path B without durable candidate_id illegal; `journaled-but-not-Started → replay-to-N only, ids never reclaimed` (L4 §10.1 / §6.1.3).
   - **Journal↔apply closed loop (round-42)** → journal admit enforces same size/type/schema as `append_seal_journal_apply`; `>kSealJournalMaxEmbeddedBytes` never journal-Acked; oversize somehow present → Corrupt/fence not permanent silent deadlock (L4 §10.1 / §6.1.3).
   - **Embeddable allowlist (round-42)** → `is_seal_journal_embeddable_type`; `record_type == embedded_type`; bridge/override/nested apply → Corrupt (L4 §10.1 / §6.1.3).
   - **Journal de-dup key (round-41)** → index `{candidate_id, journal_seq}` only; same key different `entry_mac` → `Corrupt` (reachable); indexing by key that includes MAC is non-compliant (L4 §10.1 / §6.1.3).
   - **SealJournalAppliedView ABI (round-41…46)** → `append_seal_journal_apply(const SealJournalAppliedView&)` with `std::span` payload + durable time_* + baseline + `kek_key_id`; **MUST recompute entry_mac**; sink copies under Ack; span valid until return; `kSealJournalMaxEmbeddedBytes=4096` (L4 §10.1 / §6.1.3).
   - **Watermark-before-Started (round-41/42)** → durable watermark reserve precedes Path B and Started; Started reuses reserved ids; `Started + watermark lagging → fence/Corrupt / forward-only — never reallocate` (L4 §10.1 / §6.1.3).
   - **Atomic seal-journal apply (round-40)** → only `append_seal_journal_apply` (origin+payload one frame); `Ack → crash before journal clear → single apply`; two-phase business-then-marker forbidden; recovery/fold walks embedded payloads (L4 §10.1 / §6.1.3).
   - **Outbox drain before seal (round-40)** → pause producer + empty `ExportOutboxRing` + finish tip RPCs before `SealExportStarted`; `ring non-empty → refuse started`; after tip@N+1 residual gen-N never sent; remote hard-rejects lagging tip (L4 §10.2.1 / §6.1.3).
   - **Windows breadcrumb honesty (round-40)** → complete MAC-valid `SealExportStarted` ⇒ PostSeal; undecidable dir-flush must not PreSeal-abandon (L4 §10.1 / §10.3 / §6.1.3).
   - **SealQueryStatus (round-39)** → `query_seal_by_request_id` returns `Found`/`NotFound`/`TransportUnavailable`/`Corrupt` (not `RecoveryScanStatus`); `TransportUnavailable → later Found → must NOT PreSeal-abandon`; authenticated `NotFound` may abandon **only with tip==baseline** (L4 §10 / §6.1.3).
   - **SealIdWatermark / no request_id reuse (round-39/41/42)** → ids reserved before Path B; restart reusing past `request_id` → refuse/`Corrupt`; `Found` field-binds breadcrumb + local N+1 + full baseline 4-tuple (L4 §10.1 / §6.1.3).
   - **Handoff SPSC (round-39)** → multi-writer into one SPSC forbidden; true single producer or per-producer SPSC array; full → stop read + retain buffer (L4 §10.1 / §6.1.3).
   - **Unavailable≠absent (round-38)** → kept as `TransportUnavailable` semantics under `SealQueryStatus` (L4 §10.1 / §10.2 / §6.1.3).
   - **Tip-export drain before SealExportStarted (round-38/40)** → extended to full ring empty + producer pause; seal wait is direct `ExternalAnchorClient` call (L4 §10.2.1 / §6.1.3).
   - **SealExportStarted crash window (round-37)** → `SealExportStarted` fsynced → remote seal `Found` → power-loss before local Ack → recovery → tip+flip+journal→N+1 → still frozen (L4 §10.1 / §10.2 / §6.1.3).
   - **PostSeal journal reserve / saturation (round-37)** → quiesce+drain before started; reserve held; `journal saturation + seal in-flight + 429 + power-loss` → no drop, no path A, fenced or still frozen after recovery (L4 §10.1 / §6.1.3).
   - **Seal quiesce PreSeal/PostSeal (round-35/36/37)** → volatile buffer forbidden; busy/`Failed`-without-durable for received 429 illegal. **PreSeal** (no complete MAC-valid `SealExportStarted`): A or B under B-sticky; `PreSeal 429 → A abandon → power-loss → still frozen` (A only with empty journal). **PostSeal** (from `SealExportStarted`): B only; `PostSeal 429 → journal → tip → flip → replay → still frozen`; path A after started → `Corrupt` (L4 §10.1 / §6.1.3).
   - **Bridge prev_key_id + evidence key_id (round-35/36)** → compaction/recovery bind `prev_key_id`; `CompactedFreezeWaitEvidencePayload.source_baseline_key_id` required; omit/mismatch → abandon/`Corrupt` (L4 §10.2 / §6.1.3).
   - **Compaction yield tip pin (round-34)** → retain-scan → yield → freeze/OrderEvent append on N → tip ≠ baseline → abandon N+1 → rebuild; no event loss; stale gen1 Satisfy must not seal/flip (L4 §10.1 / §10.2 / §6.1.3).
   - **Generation exhaustion (round-34)** → `CURRENT == UINT32_MAX` → compaction refused; fence (L4 §10.1).
   - **CompactedFreezeWaitEvidence single payload (round-34)** → one frame layout; dual Arm+Satisfy compaction substitute Rejected (L4 §10).
   - **Sequence-proven rewrite Arm-after-rematch (round-33)** → `freeze#1 → Arm → freeze#2 → short-wait Satisfy` must not rewrite; both Arm.seq and Satisfy.seq after every wait-bearing freeze required; else drop → re-Arm → full wait (L4 §7.3.1 / §10).
   - **Compaction snapshot ABI (round-33)** → folded `wait_generation > 1` retain uses `append_compacted_freeze_snapshot` (succeeds); same payload via live `append_rate_freeze` Rejects; compacted wait evidence creates no `arm_ack_steady` (L4 §10 / §6.1.3).
   - **Legacy upgrade rematch (round-32)** → pre-`wait_generation` log `429 wait → Satisfy → 429 wait → upgrade/restart` → recovery `out_has_wait_satisfied=false` → migration/compaction seals explicit gen `≥1` → re-Arm → full wait → Satisfy before probe/clear; compaction must not solidify legacy Satisfy after a later wait-bearing freeze (L4 §7.3.1 / §10 / §6.1.3).
   - **Equal-wait rematch / wait_generation (round-31)** → `unknown 429 wait=120 → WaitSatisfied → new unknown 429 wait=120 → old WaitSatisfied rejected → re-Arm → full wait → Satisfy` before probe/clear; compaction drops stale-generation Arm/Satisfy; recovery `out_has_wait_satisfied` false for stale gen (L4 §7.3.1 / §10).
   - **Deadline-only merge does not advance wait_generation** → after WaitSatisfied, UTC-only merge copy-forwards gen; existing WaitSatisfied remains valid (L4 §7.3.1).
   - **Failed clear does not strand WaitSatisfied** → clear Rejected leaves current-gen WaitSatisfied reusable (L4 §10).
   - **时钟跨边界 header** → `BucketIdentity::Unknown` hold, no wipe-on-rotate (L4 §7.1).
   - **快时钟 crash restart** → wall +1h still waits full steady duration; zero packets (L4 §7.0 / §7.5.1).
   - **正常取消轮询** → `Accepted`+`CANCELED` → `Cancelled`; same-state observations do not call `transition_to`; cancel-failure reverse edges (L4 §6.6 / §4.3).
   - **anchor 不可达 override + degraded hard-lag** → sidecar binds `LastRemoteAckedTip`; crash→restart→new-override cannot reset 256-frame budget; over-limit → ReadOnlyDrain (L4 §10.2 / §10.2.1).
   - **Phase-B freeze kinds** / **Permanent not forgotten** / **UTC→unknown merge** (L4 §7.3 / §7.3.1).
   - **Dual-deadline Arm+WaitSatisfied** → Arm→immediate Satisfy rejected by sink elapsed; early `/time` cannot `ProbeVerified`; must Arm→wait→Satisfy first (L4 §7.3.1 / §10).
   - **Probe Ack-before-send** → send without attempt Ack illegal; kill after Ack burns ordinal; no 8-cap launder (L4 §7.3.1).
   - **Probe deadline durable (round-27)** → multi-day deadline + crash loop cannot burn 8 probes on short backoff alone; restore folds deadline into not_before (L4 §7.3.1).
   - **No freeze TimeResync bypass (round-28)** → kill -9×N during active freeze cannot get N free `/time` via TimeResyncCredit; only durable FreezeProbeAttempt under 8-cap (L4 §7.1 / §7.3.1).
   - **ClockRepublishOrVerify last-ordinal clear (round-29)** → attempts=7 → crash loses clock → ordinal 8 → serverTime≥deadline (+WaitSatisfied if required) → FreezeClear Ack → restart not frozen; wait>0 refuses probe until WaitSatisfied (L4 §7.3.1).
   - **Probe sink wait-gate (round-30)** → wrong `wait_ok=true` → append_freeze_probe_attempt rejects → zero `/time` (L4 §7.3.1 / §10).
   - **Probe sink ordinal** → duplicate/gap/wrong-epoch / not_before<deadline append Failed (L4 §10).
   - **Probe credit restore + backoff** → attempts=7 → crash → restore re-applies backoff+deadline → 8th only after not_before (L4 §7.3.1).
   - **Epoch bind-only** → foreign epoch reserve refuses; only durable new-episode bind resets (L4 §7.3.1).
   - **Proof host ABI** → missing/mismatched `tls_verified_host` → clear rejected (L4 §10).
   - **Forged ProbeVerified** / **Typed permanent clear** (L4 §10).
   - **Clock seq / arithmetic** → `UINT32_MAX` publish refuse; overflow → hold/refuse sign (L4 §7.1.2).
   - **GenerationSeal tip before CURRENT** / **跨 generation rollback** (L4 §10.2 / §10.3).
   - Also: format v3; representability permanent fence; Windows dir-flush; `-1021` zero re-POST; TLS/DNS matrix.
