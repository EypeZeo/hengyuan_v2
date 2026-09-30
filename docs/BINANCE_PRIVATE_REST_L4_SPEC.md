# Binance Private REST — L4 Foundation Spec (Implementation Blueprint, rev 72)

## Status

**Revision 72, accepted as the implementation blueprint.** Spec-level findings are closed; implementation-level closure is defined exclusively by the fault-injection matrix at the end of this file passing against real code. §10's ABI surface has been ported to `native/include/hengyuan/durable_control_plane.hpp` (tracked in `docs/SPEC_INVARIANTS.md`), and the signed read-only client itself (`binance_private_rest.hpp`: `fetch_account`/`query_order`/`create_listen_key`/`keepalive_listen_key`/`close_listen_key`) is implemented and unit-tested — **this does not itself constitute the fault-injection matrix's implementation-level closure**, which remains a separate, not-yet-audited exercise (see this document's matrix at the end of the file). Per `docs/NATIVE_ARCHITECTURE.md`'s gate table, everything in this spec is **L4** — signed *read-only* requests plus the durable control-plane store those reads need (rate-limit freeze, symbol-registry snapshots). Nothing here submits an order; `docs/SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` (L5, POST) depends on this spec's primitives and must not be implemented before this one.

This spec exists because the original L5-only draft was rejected on review (see that file's changelog) for treating reconciliation, environment binding, signing discipline, and rate-limit accounting as someone else's problem "for later." They aren't — they're the shared foundation both reconciliation-after-ambiguous and the POST adapter need, and per ADR-019 D8, reconciliation specifically cannot be deferred past the first live submit path.

Revision 2 fixed 3 P0 and 2 P1 findings from the second Architect review round: exhaustive reconciliation-query failure semantics (§6), concrete `rules_version` data model and call-chain wiring (§5), factory-only `EnvironmentBinding` construction (§1), and a concrete `RequestWeightTracker` correction/freeze API + order-count bucket (§7).

Revision 3 fixed 4 more findings from round 3, two of them genuine safety bugs rather than missing detail:

- §6.5 — round 2's "`Found` → transition to `Reconciled`" would mark a still-open order (Binance status `NEW` or `PARTIALLY_FILLED`) as **terminal**, since `Reconciled` is one of `order_lifecycle.hpp::is_terminal()`'s terminal states. That stops the system from tracking a position that is, in fact, still live on the exchange — a real correctness bug, not just a modeling gap. Fixed: `Found` now maps to the actual confirmed state (`Accepted`/`PartialFill`/`Filled`/`Rejected`/`Cancelled`/`Expired`), and in-flight release happens only when that confirmed state is actually terminal.
- §6.2 — the retry loop's location was ambiguous, and nothing defined who increments/persists `OrderRecord::query_attempts` (the field `determine_reconcile_action()` actually reads). Fixed: the loop lives in the orchestrator, not inside `query_order()`; every attempt increments and durably persists the count before the next decision is made, and `recovery_scan()` must reconstruct it after a crash.
- §7 — order-count limits have multiple simultaneous intervals (10s, 1d, etc.); "a second tracker" can't represent that. Fixed: a small fixed-capacity tracker set, one per interval Binance actually reports.
- §1 — `EnvironmentBinding`'s factories still accepted a caller-supplied credential env-var name, which could itself be the wrong environment's name. Fixed: factories take no parameters; both the API-key and secret env-var names are hardcoded per environment.

**Revision 4** (this revision) fixes 3 P0 and 1 P1 finding from round 4:

- §6.1/§6.1.1 (P0) — `ReconcileQueryResult` had no way to carry fill data; a `Found` result confirming `PartialFill`/`Filled` had nothing to populate `OrderRecord::filled_qty_ticks`/`avg_fill_price_ticks` with. Fixed: added `filled_qty_ticks`/`avg_fill_price_ticks` fields, derived losslessly from `executedQty`/`cummulativeQuoteQty` via integer division, never floating point.
- §6.2 (P0) — the reconciliation loop's pseudocode called `append_durable()` at three points but never checked the return value, exactly the bug round 3 fixed for the submit path (`SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` §6.2) but missed here. Fixed: every `append_durable()` call in the loop now branches on `AuditAppendResult`, rolling back the in-memory `query_attempts` increment and escalating out-of-band on an unconfirmed write, rather than silently proceeding as if the write had succeeded.
- §6.2 recovery (P0, cross-file) — `recovery_scan()` could only repopulate bare `InFlightRegistry` membership, with no way to carry `query_attempts` or fill data. Fixed in `SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` §6.1 with a new `RecoveredOrderRecord` struct and a fixed-capacity output-buffer signature.
- §7.1 (P1) — `OrderCountTrackerSet::correct_from_header()` returned `void`, with no testable capacity-exhausted path, and nothing configured the actual per-interval *limit* (headers report only the *used* count). Fixed: `correct_from_header()` now returns `OrderCountCorrectionResult`; a new `configure_limit()` method sources the limit from `exchangeInfo`'s `rateLimits[]`; `can_send_all()` fails closed permanently once capacity is exhausted.
- §4 (P1) — `GET /api/v3/account`'s response handling was a one-line "populates `AccountSnapshot`" with no schema, conversion, or capacity contract. Fixed with §4.1–4.4: exhaustive schema validation, lossless decimal→tick conversion, capacity-overrun rejection (never silent truncation), and a single malformed-response fail-closed outcome.
- The `-1021` (P1, cross-file) finding is fixed entirely in `SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` §4.5 — this file's §2.2 now explicitly scopes its existing "-1021 forces a resync" rule to read-only GETs and points to §4.5 for the POST-specific handling, since a POST retry is not side-effect-free the way a GET retry is.

**Revision 5** (this revision) fixes 4 P0 and 4 P1 findings from round 5 — the round-5 review found genuine implementation-level bugs (verified against the actual `transport_policy.hpp`/`account_truth.hpp` source, not just spec-level gaps) in addition to spec-completeness issues:

- §7.1 (P0, real shipped bug) — `OrderCountTrackerSet` reused `RequestWeightTracker` "as-is" per interval, but `RequestWeightTracker`'s existing source hardcodes a 60-second sliding window — a `1D` order-count bucket would locally release its budget after 60 real seconds (fail-open: Binance's daily cap silently bypassed), and `10S` would be timed equally wrong. Fixed: `RequestWeightTracker::reset()` gains a `window_seconds` parameter (proposed change to the existing class, defaulted to 60 to preserve current per-minute-weight behavior at existing call sites); a new `parse_interval_seconds()` derives each interval's real window from its header suffix.
- §5.1.1 (P0, real shipped bug) — `account_truth.hpp::checked_notional()`'s own comment admits "caller must ensure same scale," which `validate_pre_trade()` never actually ensures: the raw `price_ticks * qty_ticks` product (scale = `price_scale + qty_scale`, symbol-dependent) was compared directly against `min_notional_ticks`/exposure caps *and* against `AssetBalance::free_ticks` (fixed 8-decimal scale per this revision's §4.2) — two different scale domains, one variable. Fixed: a new `rescale_notional_ceil()` normalizes the product to `kBalanceScale` (8) once, immediately after computing it, via lossless integer arithmetic with an explicit conservative (ceiling) rounding rule; every subsequent comparison uses only the normalized value.
- §5.3 (P0, TOCTOU) — Gate 1's `rules_version` check (round-4's fix) only compares two integers at one instant; nothing prevented `submit_order()`'s wire-formatting step, several gates later, from independently re-querying the registry and picking up a newer entry an operator refresh published in between — round 4 moved the check earlier but didn't close the window, the same category of fix round 3 found insufficient for `RateLimited`. Fixed: the orchestrator captures the actual `SymbolRules` value (not just its version) once, at pre-trade-validation time, and carries that snapshot — not a version integer — all the way to `submit_order()`/`SubmitFn`, which performs no registry lookup of its own. Gate 1's integer comparison is retained only as a cheap fast-reject, not as the source of correctness.
- §6.1/§6.1.1/§6.1.2 (P0, cross-file) — `RecoveredOrderRecord` (added round 4) still couldn't reconstruct enough of an order to resume managing it (no side/type/intended price&qty/submit time/rules snapshot), `recovery_scan()` couldn't distinguish "nothing in flight" from "the log is corrupt/unreadable," and the underlying `AuditRecord` never carried the fields recovery needs to replay in the first place. All three fixed in `SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` §6.1/§6.1.1/§6.1.2 — an extended `RecoveredOrderRecord`, a `RecoveryScanStatus` enum with explicit Corrupt/CapacityExceeded/IoError fail-closed cases plus a defined record-framing/checksum/truncated-tail contract, and 4 new `AuditRecord` fields.
- §6.1.2 (P1) — `Found` results were only checked against a matching `origClientOrderId`, not against symbol/side/type/price/origQty — the same class of gap round 3 already fixed for the direct POST response (`SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` §4.2), missed here. Fixed with the same two-step schema+field-match discipline.
- §7.3 (P1) — `Retry-After` had no defined parsing contract; missing/malformed/negative/zero/absurd values were all previously undefined behavior at every 429/418 call site. Fixed with `parse_retry_after()` and explicit fail-closed defaults (60s missing/malformed, 3600s ceiling).
- §8 (P1, real gap) — the response-size cap (`check_response_size()`) was checked only *after* `binance_rest_snapshot.hpp`'s existing read pattern (`http::read` with an unconfigured `string_body`) had already buffered the full response into memory — the cap couldn't prevent the allocation it exists to prevent. Fixed: `body_limit()` configured on the parser before reading. Also fixed: `submit_order()`/`query_order()` are declared `noexcept` but the Beast/Asio/OpenSSL/simdjson machinery beneath them can throw — an escaping exception would call `std::terminate()`; both must wrap their bodies in try/catch matching `binance_rest_snapshot.hpp`'s own existing pattern.
- §9 (P1, new section) — no revision had defined who owns `RequestWeightTracker`/`OrderCountTrackerSet`/`SymbolRegistry`/`DurableAuditSink` when submit, reconciliation, and account/registry refresh can all touch them; none of the existing tracker code is internally synchronized. Fixed: single-owner-thread model (all L5 gate-chain execution on one dedicated non-hot-path thread), with `SymbolRegistry`'s existing `std::shared_mutex` kept as the one deliberate exception for the operator-refresh thread boundary.
- §4.4 (P0, cross-file) — HTTP 403 was classified as a definitive `Rejected` ("blocked before the matching engine") with no Binance documentation actually guaranteeing that — the same unverified-assumption error round 3 found in the original `RateLimited`/`-1021` framing. Fixed in `SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` §4.4: 403 now routes through `Ambiguous`.
- §6 (P1) — this file's own `GET /api/v3/order` example query string was `symbol&origClientOrderId&recvWindow&timestamp`, not alphabetical, contradicting §2.1's own canonical-order rule. Fixed.

**Revision 6** (this revision) fixes round 6's 8 P0 + 8 P1 findings — the most severe of which was a self-inflicted bug (§6.1.2) that would have made reconciliation's `Found` outcome unreachable in practice:

- §6.1.2 (P0, most severe) — revision 5 required the `GET /api/v3/order` **response** to contain `origClientOrderId` as a schema field; that field name is a **request parameter**, not a response field (the response identifies the order via `clientOrderId`). Every legitimate response would have failed this schema check → `Inconclusive` → escalate, unconditionally. Fixed: corrected field name, plus `query_order()` now takes a full `OrderExpectation` (side/type/price/qty/rules-scale) instead of just an ID, since round 5's "cross-check the response" was also unimplementable without it — `OrderRecord` gains the fields needed to build one.
- §4.4.1 (P0) — `-2010` was classified `Rejected`; Binance documents it as covering duplicate-`clientOrderId` cases too, where the order might actually be live. Fixed: routes to `Ambiguous`.
- §6.1.1 (P0) — the `avg_fill_price_ticks` formula silently assumed `cummulativeQuoteQty` was scaled at `price_scale`; it's actually scaled at a separate, symbol-specific `quote_scale` (new `SymbolRules` field). Fixed with the correct dimensional derivation and a checked, signed-exponent scaled-mul-div helper.
- §5.1.1 (P0) — `rescale_notional_ceil()`'s own overflow-avoidance had two overflow bugs: an unbounded `pow10_i64()` and a ceiling-division idiom that itself overflows near `INT64_MAX`. Fixed with a bounded `pow10_i64()` and a quotient/remainder ceiling that can't overflow.
- §6.1.1/§6.1.2 (P0, cross-file) — `RecoveredOrderRecord`'s promised recovery (fill data, full `SymbolRules`, `query_attempts`) had nowhere to live in the fixed-size `AuditRecord`. Fixed with a second, variable-length `SymbolRegistrySnapshot` durable-record type.
- §6.1.1 (P0) — the "discard the last record on checksum failure" rule didn't distinguish a torn write (safe to discard) from a complete-but-corrupted record (never safe to discard, even if it's last). Fixed with an explicit physical-presence check first.
- Post-response durable-ACK gap (P0, cross-file) — no revision required the `Accepted`/`Rejected`/`Filled`/`PartialFill` outcome itself to be durably ACKed before releasing in-flight/transitioning state. Fixed in `SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` §6.4.
- §7.1/§7.2 (P0) — `OrderCountTrackerSet::can_send_all()` was a pure check with no reservation, and fresh-process trackers assumed zero prior usage even for account-wide, restart-surviving intervals. Fixed with `try_reserve_all()` (reserve-before-send, never rolled back) and an explicit startup-baseline rule for long intervals (§7.1.1).
- §7.1 (P1) — long intervals (`1D`) modeled as a purely local relative sliding window; refined to anchor to server-time-aligned UTC boundaries.
- §7.3 (P1) — `Retry-After` values were clamped to 1 hour and then actively retried, unsafe against Binance's documented multi-day 418 bans. Fixed: an absolute 64-bit deadline, honored exactly, never clamped; permanent fail-closed on the (unreachable in practice) unrepresentable case.
- §6.5 (P1) — only `NEW`/`PARTIALLY_FILLED`/`FILLED` were discussed by name; `PENDING_CANCEL`/`EXPIRED_IN_MATCH` had no defined mapping. Fixed with an exhaustive status table.
- §8 (P1) — per-phase deadlines were promised but `PrivateRestConfig` only had 2 timeout fields; fixed with 5 explicit phase deadlines plus a cancel/no-reuse-after-timeout rule, in `SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` §2.
- §9.1/§9.2 (P1, new) — the single-owner thread (round 5) would starve control-plane/refresh tasks if implemented as a blocking loop with inline sleeps; fixed with an actor/scheduled-task model. Cross-thread publish semantics for `current_rules_version()` and the clock-offset resync were also left undefined; fixed (shared registry lock reused; offset as a single `std::atomic`).
- §8 (P1) — `body_limit()` covered the body only; header-size limit, no-compression, and force-close-after-exception were all unaddressed. Fixed.
- Durable log encoding (P1) — endianness/padding/enum-range/version-migration/single-writer/flush-deadline/tamper-evidence were all unspecified; fixed in `SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` §6.1.1.1, including upgrading the checksum from CRC32C to HMAC-SHA256 for genuine tamper evidence, not just corruption detection.

**Revision 7** (this revision) fixes round 7's 8 P0 + 8 P1 findings — round 7 dug specifically into failure-injection scenarios (deletion, rollback, crash-loops, corrupted lengths) and two factual errors from round 6's own reasoning:

- §6.1.1.1 (P0, cross-file) — round 6's HMAC covered only `length + payload`, leaving `version`/`type` unauthenticated and, critically, providing no defense against deleting an entire valid frame from the log (every remaining frame still verifies). Fixed in `SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` §6.1.1.1 with full-frame MACs, a monotonic-sequence hash chain, and a persistent chain-tip anchor stored outside the append-only log.
- §5.3.1 (P0, new) — `refresh_from_exchange_info()` never specified whether the durable `SymbolRegistrySnapshot` write happens before or after the in-memory registry starts publishing the new `rules_version`; an order could reference a version with no durable snapshot behind it. Fixed: durable ACK is now a precondition of publication, not a separate, reorderable step.
- §6.1.3 (P0, cross-file) — the variable-length `SymbolRegistrySnapshot` frame (round 6) had no `symbol_count` bound, no checked-offset-arithmetic requirement, and no bound on the recovery-time rules_version index — a corrupted length field could OOM or hang recovery. Fixed in `SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` §6.1.3.
- Post-response-audit-failure fallback (P0, cross-file) — an earlier revision left the order at `Submitting` when the post-response audit write failed; `order_lifecycle.hpp::determine_reconcile_action()` only acts on `Ambiguous`, so the order would never be picked up again. Fixed in `SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` §6.4 (in-process) and §6.1 (recovery-time — a recovered "Submitting" state is now remapped to `Ambiguous`).
- §7.1 (P0, factual error) — `configure_limit()` filtered `exchangeInfo`'s `rateLimits[]` for `rateLimitType=ORDER_COUNT`; Binance's actual value is `ORDERS`. The filter would match zero entries, leaving `OrderCountTrackerSet` empty — and an empty interval set made `can_send_all()` vacuously return true (nothing to check = nothing objects). Fixed: corrected the type name, and made an empty set explicitly fail closed rather than vacuously pass.
- §7.1.1 (P0, factual error) — round 6 claimed no Binance endpoint reports order-count usage without consuming any, and built a self-contradictory "block long intervals but still allow a blind first send" rule around that claim. Binance actually publishes `GET /api/v3/rateLimit/order` for exactly this. Fixed: L5 startup now calls it and blocks entirely on failure — no more blind first send.
- §6.1 (P0, cross-file) — `ReconcileQueryResult`/`SubmitResponse` still carried `retry_after_seconds` (a `uint32_t` duration) despite §7.3's `parse_retry_after()` (round 6) producing an absolute deadline — a real ABI mismatch that would force reconstructing a deadline from a duration at a later, possibly-delayed point. Fixed: both structs now carry the absolute deadline directly.
- §7.2 (P0, cross-file) — `RequestWeightTracker`'s side of Gate 8 still used the older, non-reserving `can_send()`, and the `-1021` retry path (`SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` §4.5) re-checked non-reserving functions entirely, bypassing reservation. Fixed: a single `try_reserve_all_budgets()` combining weight + order-count reservation with rollback-on-partial-failure, used at every order-placing send site including the `-1021` retry.
- §2.2/§8/§9.1 (P1) — DNS-resolve cancellation, TLS minimum version/CA-store policy, and reconciliation's attempt-then-persist ordering (a crash loop could send unbounded real queries while `query_attempts` never advanced) were all fixed.
- §6.1.4 (P1, cross-file) — `query_order()`'s separate `symbol` parameter could drift from the captured snapshot; `timeInForce` was schema-required but never field-matched; replay had no defined behavior for duplicate/out-of-order/contradictory events. All fixed — `query_order()` now derives `symbol` from the snapshot alone, `timeInForce` is field-matched, and replay walks `validate_transition()` itself.
- §5.1.1 (P1) — `rescale_notional_ceil()`'s `std::uint8_t` scale parameters could silently narrow-wrap a corrupted `price_scale+qty_scale` sum. Fixed with `int` parameters and an explicit range check.
- §2.2 (P1) — clock-offset freshness was TTL-only, which doesn't detect a wall-clock step change (NTP correction, VM pause/resume) within the TTL window. Fixed with a wall-clock/monotonic-clock consistency check performed at every signed-request preparation.

See `SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md`'s revision 8 changelog for the full point-by-point mapping — both files were revised together in response to the same review.

**Revision 8** (this revision) fixes round 8's 7+ P0 + P1 findings — failure-injection against the *already-fixed* revision-7 design, plus deeper bugs the review did not list:

- §6.2 (P0) — `schedule_reconcile_retry_at()` did not `return`/`break` from `while(true)`, so backoff/`Retry-After` were scheduled but the next GET fired immediately, burning `kMaxQueryAttempts` in a tight loop. Also used ternary select instead of the `max(backoff, retry_after)` §6.3 already required. Fixed: schedule then `return`; deadline is `max(...)`.
- §7.1 (P0) — short intervals still used `RequestWeightTracker`'s relative sliding window; only `>= 3600s` got UTC alignment. Binance documents fixed, server-aligned buckets for *all* intervals (`10S` resets at :00/:10/:20…; `1M` at minute boundaries; `1D` at 00:00 UTC). Sliding decay locally releases budget early inside a still-active server bucket. Fixed: every interval uses server-aligned fixed-bucket rotation; ORDERS count is never locally decremented on assumed fills (only response headers / `GET /api/v3/rateLimit/order` may lower it — fills decrement unfilled count server-side with delay).
- §7.3.1 (P0, new) — 429/418 freeze was memory-only (`TimePoint`); a process restart during an active ban could immediately re-send and deepen a 418. Fixed: durable `RateLimitFreeze` frame (L5 §6.1.2) carrying a UTC wall-clock deadline; recovery restores the freeze before any signed send is permitted. Steady-clock deadlines are in-process only and are re-derived from the UTC deadline at restore time.
- §7.4 (P0, new) — request-weight pre-send reservation existed only for order-placing `try_reserve_all_budgets()`. `/api/v3/account`, `GET /api/v3/order`, `GET /api/v3/rateLimit/order`, and `exchangeInfo` each have distinct documented weights and could punch through IP `REQUEST_WEIGHT` before any response header arrived. Fixed: compile-time endpoint-weight table + `try_reserve_weight_only()` on every signed/public REST send path.
- §6.6 (P0, deeper) — after `Found → Accepted`/`PartialFill`, `determine_reconcile_action()` returns `NoAction`, so resting live orders would never be polled for fills again (no user-data stream in this design). Fixed: open-order status-poll schedule, separate from Ambiguous's escalate-at-3 counter.
- §7 (P1) — `reset(..., window_seconds, now)` broke existing `reset(limit, margin, t0)` test call sites (3rd arg is `TimePoint`). Fixed: `window_seconds` is the 4th parameter.
- Cross-file P0s (recovery replay, per-ACK anchor, `append_snapshot`, flush fencing, registry-version compaction, L5 gate-list joint reservation, L5 "persist attempts after query" contradiction) — fixed in `SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` revision 9; see that changelog.

**Revision 9** (this revision) fixes round 9's 9 P0 + P1 findings — including a structural cycle (L4 freeze depended on L5 types) and several "defined in prose, unimplementable against the ABI" gaps:

- §7 (P0) — fixed-bucket rotation required server UTC, but every tracker method only took `steady_clock::TimePoint`. Fixed: all consuming/correcting APIs take an explicit `server_now_ms`; missing/stale clock-offset → fail-closed refuse.
- §10 (P0, new) — `RateLimitFreeze` / `append_rate_freeze` / `append_snapshot` sink into this L4 durable control-plane foundation so account refresh and startup baseline no longer reverse-depend on L5. Breaks the L4↔L5 cycle.
- §9 (P0) — operator registry refresh must enqueue onto the L5/L4 actor; never call `append_snapshot` from a foreign thread. OS file locks do not serialize same-process threads.
- §6.2 (P0) — weight reservation failure after `query_attempts++` falsely burned the escalate budget. Fixed: reserve first, then persist attempt, then GET; reservation failure does not consume an attempt.
- §7.5 (P0, new) — track `RAW_REQUESTS` from `exchangeInfo` (was explicitly deferred → real 429 risk).
- §1.1 (P0, new) — exclusive egress-IP deployment constraint; single-writer durable store is necessary but not sufficient against shared-NAT IP quota.
- §7.4 (P1) — concrete pinned weights + versioned override map with safety margin (declaration-only table was unimplementable).
- Cross-file P0s (PartialFill state machine, OrderSubmitted failure compensation, compaction retaining active freezes, replay consistency, intent orphans, compaction atomic switch) — fixed in L5 revision 10.

**Revision 10** (this revision) fixes round 10's 8 P0 + P1 findings — several of which were *conflicts between round-9 fixes themselves* (the universal-reservation rule vs. the data reservation needs; per-ACK anchor vs. whole-store rollback; per-second polling vs. the weight budget it must respect):

- §7.0 (P0, new) — **cold-start deadlock**: round 9 required reservation before *every* REST call, but the trackers' configuration (`exchangeInfo`) and `server_now_ms` (from `/time`) only exist *after* those first calls — first startup could never legally send anything. Fixed: a bounded, explicitly-defined bootstrap phase with pinned conservative limits, a no-rotation (strictly accumulating) bucket rule while server time is unknown, and a fixed whitelist of bootstrap-only endpoints. No circularity remains: bootstrap needs no server data to be safe.
- §7.1.2 (P0, new) — `server_now_ms = local + offset` is an RTT/2 estimate with no error bound; near a `10S`/`1M`/`1D` boundary a fast local clock could rotate the bucket **early** and spend budget the server hasn't granted. Fixed: the offset publishes an explicit uncertainty bound; bucket rotation happens only at `boundary + error_bound` (late, never early), and no "rotate early" path exists.
- §6.6 (P0) — the flat 1-second poll cadence is arithmetically impossible at capacity (64 in-flight × weight 4 × 60/min = 15,360 weight/min, above the entire documented 6,000/min IP budget) and a single-actor GET can block to its deadline. Fixed: a global open-order poll **weight budget** (fraction of the configured limit), admission by round-robin queue with priority below reconciliation/control-plane, cadence as a *floor not a guarantee*, and a staleness alarm (never silent starvation).
- §7.5.1 (P0, new) — RAW_REQUESTS/REQUEST_WEIGHT had a limit but no restart *baseline*; Binance provides no usage-report endpoint for them (unlike ORDERS). A crash-restart inside a consumed bucket started local counts at 0 — fail-open. Fixed: crash restart waits out the longest IP-scoped window (rollover + error bound) before any non-bootstrap send; a clean shutdown may instead persist a `RateLimitUsageSnapshot` frame and resume from it conservatively.
- §10 (P0) — the store detected frame deletion/truncation but treated **delete-everything** (log+anchor+meta) as a fresh `Clean` start, and a substituted older self-consistent store was undetectable. Fixed: explicit threat-model statement (what local-only can and cannot detect), a store-identity breadcrumb kept *outside* the store root (fresh-looking store + existing breadcrumb → `IoError`, fail closed), and a mandatory-for-production external tip-anchor export with a defined recovery cross-check and a defined unreachable-channel degradation path.
- §10 (P0) — `AuditAppendResult`/`RecoveryScanStatus` were "defined canonically in L4" *and* "mirrored" in L5 — same-name enums in two headers is an ODR violation and an ABI-drift vector. Fixed: exactly one C++ definition in one named shared header (`hengyuan/durable_control_plane.hpp`); L5's spec text may *quote* it but must include, never redefine.
- §10 (P1→ABI) — `EndpointWeightConfigSet` had a record type and a recovery output but no append ABI; same for the new usage snapshot. Fixed: `append_weight_config()` / `append_usage_snapshot()` added to `DurableControlPlaneSink`.
- §10 (P1) — store size was unbounded (unbounded startup HMAC scan / disk exhaustion). Fixed: hard byte/frame caps, high-watermark compaction scheduling, and writer-fencing at the hard cap. Anchor/`CURRENT` replacement now has an explicit per-platform atomic-replace + directory-durability contract.
- Cross-file P0s (progressive-PartialFill replay corruption, `-1021` re-POST self-contradiction, escalated-order capacity exhaustion, gate-order for 429/418 bodies) — fixed in L5 revision 11; §6.2/§6.6/§7.2 here reference the corrected semantics.

**Revision 11** (this revision) fixes round 11's P0s — deep consistency holes inside the *conservative* round-10 design (torn atomic pairs, header/bucket mismatch, crash-path bootstrap still punching the IP budget, steady/UTC deadline confusion, poll-failure crash amnesia, incomplete ABIs, fence-vs-reconcile contradiction):

- §7.1.2 (P0) — `offset_ms` + `offset_error_bound_ms` as two independent atomics still allows a reader to observe **new offset + old/smaller bound** → early rotation. Fixed: a single versioned `ClockOffsetSnapshot` published under a seqlock (or equivalent atomic snapshot); readers retry until a consistent pair; rotation never uses a torn pair.
- §7.1 (P0) — header correction had no server-bucket identity; under late rotation a server-current used-count could be applied into a still-local-old bucket and then wiped on rotate. Fixed: every correction carries `server_bucket_start_ms` (derived from response `Date` / signed server time at receipt); cross-bucket corrections are held as pending until the local tracker adopts that bucket (or force-adopt conservatively), never written into a mismatched local bucket.
- §7.0 / §7.5.1 (P0) — crash-path still permitted bootstrap `GET /time` immediately; halved bootstrap limits do not cover a predecessor that exhausted the *full* IP quota. Fixed: crash/unknown-usage path waits out the longest IP window on **local wall clock + pad** before *any* network I/O, including `/time`. Cold first-ever start (no breadcrumb/store) is the only path that may bootstrap without waiting.
- §7.3 (P0) — `RetryAfterResult::deadline_ms` was documented as steady-clock-relative yet written into `deadline_utc_ms`. Fixed: parse returns both `deadline_utc_ms` (persistence/recovery) and `deadline_steady` (in-process `freeze_until`); the two are never substituted for each other.
- §6.6 (P0) — `OrderOpenStatusPolled` was written before GET but `open_poll_failures++` only after Inconclusive; crash after a failed GET lost the failure. Fixed: persist the *incremented* failure count before GET (same discipline as Ambiguous `query_attempts`); Found durably resets to 0.
- §10 (P0) — `DurableRecordType::OrderEvent = 0` was a comment, not an enumerator — L5 range-check / write path unimplementable. Fixed: `OrderEvent = 0` is a real enumerator; L4 never *writes* it.
- §10.2 (P0) — external-anchor export failure had no backpressure; lag could grow unboundedly while local ACKs continued, silently degrading rollback protection. Fixed: export lag is hard-capped; exceeding it fences the writer. `ExternalAnchorUnavailable` is a real `RecoveryScanStatus` value with an explicit override-evidence ABI.
- Cross-file P0s (outcome-audit-fail vs fence contradiction; `EscalatedLedger` recovery ABI missing from `recovery_scan` signature) — fixed in L5 revision 12.
- P1: production allowlist explicitly pins `api`/`api1`/`api2`/`api3` and documents why `api4`/`api-gcp` are excluded by default (operator-extensible via versioned allowlist config); signer capability gate rejects non-HMAC keys at load; tip-anchor carries `key_id` (L5 §6.1.1.2).

**Revision 12** (this revision) fixes round 12's 8 P0 + P1 findings — correctness under C++ memory model, clock-identity humility, and previously unimplementable recovery/override ABIs:

- §7.1.2 (P0) — seqlock over plain `ClockOffsetSnapshot` fields is a C++ data race / UB even if `seq` is atomic. Fixed: publish via `std::atomic<std::shared_ptr<const ClockOffsetSnapshot>>` (C++20) — immutable snapshots, no torn non-atomic fields.
- §7.1 (P0) — deriving bucket identity from the local point estimate (or untrusted `Date`) can still mis-attribute N+1 usage into bucket N near a boundary. Fixed: identity is `Trusted` only with a verified `Date` (or equivalent server-authoritative time) **and** when the estimated time is not within `error_bound` of a boundary; otherwise `Unknown` → correction held, new reservations frozen until identity resolves — never treat an estimate as identity fact.
- §7.0 / §7.5.1 (P0) — crash bootstrap wait on local UTC + 120s pad fails when the local clock is fast beyond the pad. Fixed: crash wait is a **steady_clock** duration from process start (`max_pinned_ip_window + pad`), independent of wall-clock correctness; no wall-boundary math before the first `/time`.
- §7.3 (P0) — missing/malformed `Retry-After` defaulting to 60s is incompatible with RAW 5-minute windows and multi-day 418 bans. Fixed: **418** missing/malformed → permanent operator fence (`source==2`); **429** missing/malformed → wait longest known IP bucket rollover + pad (never 60s).
- §6.6 (P0) — open-order observation matrix incomplete: same-state `Accepted`/`CancelRequested` must not call `transition_to()`; `Accepted`/`PartialFill` → `Cancelled` edges missing. Fixed with an explicit observation table + proposed state-machine edges.
- §10 (P0) — `OperatorOverride` was required but had no `DurableRecordType` / payload / append / recovery-admission ABI (self-lock). Fixed: full type + sidecar admission path that does not require writing into a store that recovery has already refused.
- §10.2 / L5 §6.1.3 (P0) — compaction generation switch lacked predecessor-MAC bridge and external-ACK-before-`CURRENT` ordering; legal compaction was indistinguishable from whole-store rollback. Fixed: `GenerationBridge` frame + external seal ACK before `CURRENT` flip; recovery **completes** an ACK'd-but-unflipped CURRENT (crash between seal ACK and pointer flip).
- §6.6 (P0, deeper) — observation matrix still omitted cancel-*failure* reverse edges (`CancelRequested` → `Accepted`/`PartialFill` when exchange returns `NEW`/`PARTIALLY_FILLED` after a failed cancel) and several illegal status regressions (must be `Inconclusive`, not stuck/`transition_to` UB).
- §6.2 / §2.2 (P0, consistency) — reconciliation task signature still took singular `RequestWeightTracker&`/`RawRequestsTracker&` while §7.2/§7.5 require Sets; §2.2 prose still said "seqlock." Fixed to Sets + `ClockOffsetPublisher`.
- §10.2 (P0, deeper) — OperatorOverride *mirror-into-log* after admission required an append ABI that did not exist; tip binding was seq-only. Fixed: `append_operator_override` + `local_tip_mac` on the payload.
- Cross-file P0 (`kMaxInFlight` dual definition / declaration order) — fixed in L5 revision 13: sole definition remains `order_lifecycle.hpp`; L5 only adds `kMaxEscalated`.
- P1: REQUEST_WEIGHT/RAW multi-interval tracker sets; poll-failure recovery by sequence replay; compaction chunk/yield; checkpoint fold bound; generation directory fsync order; hard-staleness blocks new submits.

**Revision 13** (Architect repair round 13) closes the remaining cross-file ABI holes:

- `ClockOffsetSnapshot` now carries the wall and monotonic fetch anchors required by §2.2's jump detector; `ClockOffsetPublisher::publish()` returns `bool` and never permits allocation failure to escape `noexcept`.
- Ordinary HTTP `Date` is no longer a rate-limit bucket authority. Only a dedicated `/api/v3/time` proof bound to the active offset snapshot can make a correction `Trusted`; every other response is `Unknown` and holds reservations.
- A clean-shutdown usage snapshot is seeded conservatively **before** bootstrap `/time`; `/time` can only relax the claim after confirming the server bucket. `parse_retry_after()` receives HTTP status explicitly.
- `ExternalAnchorClient` is now a concrete authenticated read/export contract. `GenerationSeal` commits the final compacted generation content root and final tip; recovery cannot auto-flip `CURRENT` without that verified seal.
- The shared durable ABI defines `OrderRecoveryCheckpoint`, checkpointed poll staleness, and canonical payload fields for all new order events. Sidecar records gain KEK identity, expiry, nonce, atomic-write/clear rules and replay protection.

**Revision 14** (this revision) fixes round 14's 7 P0 + 6 P1 findings — mostly ABI gaps the previous round's *prose* assumed but never actually gave callers a way to construct, plus one genuine self-lock and one genuine terminology bug with real safety consequences:

- §10 (P0, self-inflicted) — `OrderOpenPollFailuresReset::reset_after_seq` had no way to be obtained: `append_durable()` returned only `Acked`/`Failed`, no sequence number. Fixed: `AuditAppendResult` becomes a struct carrying `.sequence` (the assigned frame sequence) alongside `.status`; every `append_*`/`append_durable` comparison in both spec documents updated to `.acked()`.
- §7.3.1 (P0, self-lock) — recovery restoring a persisted freeze, then re-deriving `deadline_steady`/verifying a UTC deadline via `/time`, both required sending `/time` — which the just-restored freeze itself would block. Fixed: a narrow `probe_server_time_during_freeze()` path, exempt from `is_frozen()` specifically (still weight-budgeted), used only for freeze re-verification. Also fixed a wording bug calling `/time` "authenticated" when L4 §3 already establishes it as unauthenticated.
- §6.1.2/§6.1.4 (P0, cross-file, self-contradiction) — `OrderRecoveryCheckpoint::last_event_sequence` was described as both "no reference to an earlier generation" and a cross-frame sequence reference; separately, the replay loop read `record.record_type`/`record.checkpoint` with no backing variant type ever defined. Fixed: renamed to `own_sequence` (self-referential, new-generation-only) and defined `DecodedOrderFrame`, the actual tagged type replay iterates.
- §6.1.3 (P0, cross-file) — compaction's snapshot-retention rule used `is_terminal()`, which is `true` for `EscalatedToOperator` — an escalated-but-unresolved order's pinned registry snapshot could be dropped, breaking recovery's ability to interpret it. Fixed: new `is_exchange_final()` (excludes `EscalatedToOperator`), used consistently everywhere compaction decides what's safe to drop.
- §10.2 (P0) — the external-anchor anti-rollback check compared bare `sequence_number`, which a fresh post-compaction generation's genesis sequence (usually 0) would fail against the prior generation's higher sequence — every legitimate compaction would look like a rollback. Fixed: comparison is lexicographic on `(generation, sequence_number)`; a generation increase is accepted only when backed by a verified `GenerationBridge`.
- §10 (P0) — `GenerationBridgePayload::bridge_mac` was "under the tip key" with no field saying which key — unverifiable across a key rotation boundary. Fixed: explicit `prev_key_id`/`new_key_id` fields, pinned MAC domain.
- §10.2 (P0) — `OperatorOverrideSidecar`'s evidence + tombstone are both local-host-only; nothing prevents a local attacker with filesystem access from deleting both and replaying an old override. Fixed: documented explicitly as an accepted trust-boundary limitation (not a cryptographic guarantee), with tip-binding as the one mitigation this design does provide (replay against an advanced store is at least detectable).
- P1s: `last_poll_completed_ms` renamed `last_poll_completed_utc_ms` (time-basis mismatch against the durable UTC field could bypass the hard-staleness gate after a wall-clock rollback); `TimeResyncCredit` now sized from the live `EndpointWeightConfig`, not a hardcoded constant; `ClockOffsetPublisher` defaults to a mutex-guarded copy instead of `atomic<shared_ptr>` (reference-count churn on every read, not appropriate even off the hot path without profiling justification); `recover_control_plane()` gains `out_has_weights` (the one out-param missing its presence flag); `ExternalAnchorClient`'s "Acked" is now explicitly defined as remote durable confirmation, not local transport success.

**Revision 15** (this revision) fixes round 15's 3 P0 + 2 P1 findings — two of them self-inflicted by round 14's own fixes:

- §6.1.2/L5 (P0, self-inflicted) — round 14's `OrderRecoveryCheckpoint::own_sequence` was a payload field requiring the checkpoint's own not-yet-assigned sequence to already be known before the write that assigns it — circular, and unconstructible under any group-commit implementation. Fixed: removed entirely; a checkpoint's sequence is read from the frame header (`DecodedOrderFrame::sequence`) at decode time, and a later event's `event_sequence_ref` is captured from `append_order_checkpoint()`'s own `AuditAppendResult.sequence` immediately after that write completes — never predicted in advance.
- §6.1.4/L5 (P0, self-inflicted) — the same-state replay branch's `advance_replayed_fill(replayed_fill, record)` still passed the raw `DecodedOrderFrame` instead of `record.event`, inconsistent with the identical call one branch below and left over from round 14's type fix. Fixed.
- §7.3.1 (P0) — round 14's `probe_server_time_during_freeze()` still routed through the ordinary `RequestWeightTrackerSet` reservation, which itself needs a working clock offset and a configured (`exchangeInfo`-populated) tracker — exactly the bootstrap circularity §7.0 exists to solve, reintroduced for the freeze-recovery case, with no bound on retry frequency (risking ban amplification). Fixed: a dedicated, self-contained `FreezeProbeCredit` — independent of the ordinary trackers, single-flight, durably-persisted attempt count (survives a crash-loop without resetting), hard cap of 8 total attempts, exponential backoff capped at 5 minutes.
- §6.1.3/L5 (P1) — `is_exchange_final()` omitted `AbortedPreSend`, a genuinely resolved terminal state (order provably never reached the exchange) — every pre-send-aborted order would be retained forever by compaction, an unbounded storage/COID leak. Fixed: included.
- §10.2 (P1) — the relationship between `append_durable()`'s local `Acked` and `ExternalAnchorClient`'s remote-durability `Acked` was unspecified, along with FIFO/queue-full/timeout/fencing behavior — a real throughput concern if conflated (every local write blocking on a network round-trip) and a real correctness gap if left ambiguous. Fixed with new §10.2.1: two distinct roles (owner-actor local append vs. a separate export worker), local `Acked` never waits on the network, backpressure is a local FIFO-occupancy check on the append path, and compaction's `export_and_wait_ack()` remains the one deliberate synchronous-remote-wait exception.

**Revision 16** (this revision) fixes round 16's 3 P0 findings — all genuine gaps in round 15's own §10.2.1 fix:

- §10.2.1 (P0, durable-outbox gap) — a crash between a local append's `Acked` and its push onto the in-memory export FIFO silently dropped that frame from ever being exported, and from the recovery path's visibility, since the FIFO itself isn't durable. Fixed: on every startup, the export backlog is reconstructed from the local durable log itself, scanned forward from the remote-confirmed tip (`ExternalAnchorClient::read_latest_tip()`) — not trusted to have survived in the volatile FIFO — and the reconstructed backlog is checked against the hard-lag fence exactly as a live one would be.
- §10.2.1 (P0, ownership contradiction) — the prose said the export worker owns/drains the FIFO; the `ExternalAnchorClient` interface comment said the client itself maintains it — two different owners for one queue, with no SPSC/concurrency discipline defined. Fixed: the FIFO (`ExportOutboxRing`) is owned solely by the durable sink, as a fixed-capacity SPSC ring with explicit acquire/release publication on separate cache-lined head/tail indices; `ExternalAnchorClient` is redefined as a pure, stateless remote-transport client with no queue of its own.
- §7.3.1 (P0) — `FreezeProbeCredit`'s "durable, persisted attempt count" had no backing `DurableRecordType`/payload/recovery algorithm, and its `kWeightCredit=1` was a hardcoded constant that could undercut an operator-configured higher `GetServerTime` weight — a "guess downward" this design's own principle elsewhere forbids. Fixed: new `DurableRecordType::FreezeProbeAttempt` + `FreezeProbeAttemptPayload` (carrying a `freeze_epoch` so attempt counts never leak across unrelated freeze episodes), integer-table backoff (no `std::pow`, no undefined helper function), and `weight_credit()` computed as `max(pinned, recovered-config-if-known)`.

**Revision 17** (this revision) fixes round 17's 3 P0 findings — all in round 16's own fixes, plus a real compile error found and fixed while auditing them:

- §7.3.1 (P0, self-inflicted) — round 16's `FreezeProbeCredit` durable-write text called `DurableAuditSink::append_durable()` — an **L5-only** interface — for an L4-canonical record type, reintroducing the exact L4-depends-on-L5 cycle round 9 closed (this probe also runs on L4-only paths — account refresh, startup baseline — with no L5 sink to call). Fixed: new `DurableControlPlaneSink::append_freeze_probe_attempt()` (L4-owned, alongside every other control-plane append), and `recover_control_plane()` extended with `out_freeze_probe_attempts`/`out_freeze_probe_attempt_count`.
- §7.3.1/§10 (P0) — `freeze_epoch` had no durable high-water mark: compaction is explicitly allowed to drop old `RateLimitFreeze`/`FreezeProbeAttempt` records once resolved, so after such a compaction (or simply "no freeze currently active"), nothing preserved the highest epoch number ever used — a restart could reissue an already-used epoch. Fixed: new `DurableRecordType::FreezeEpochWatermark` + `FreezeEpochWatermarkPayload`, written and Acked strictly before the epoch it names is ever used, always retained by compaction (single latest record, same pattern as the latest symbol-registry snapshot).
- §10.2.1 (P0) — the durable-outbox recovery bullet said "re-populate the FIFO from this scan" with no actual bounded procedure: what happens when the backlog exceeds the ring's capacity, and is new local work admitted while still catching up? Fixed: an explicit 8-step bounded recovery-drain (scan from the remote-confirmed tip, fill the ring up to its fixed capacity, keep the scan cursor open to continuously refill as the export worker drains, and refuse all new local appends for the entire drain window) — plus the previously-undefined `ExportTuple` struct itself, which `ExportOutboxRing`/`ExternalAnchorClient` were already written against.
- **Also found and fixed while cross-referencing the above** (not from the review, from re-reading the full extent of round 16's own edit): `RateLimitFreezePayload`'s struct definition was missing its closing `};` after the `freeze_epoch` field was added — a literal compile error sitting undetected since round 16. This is exactly the kind of self-inflicted regression the review has been catching round after round; see the note left in place at that struct for what changed in how these edits get double-checked from here.

**Revision 18** (this revision) fixes round 18's 2 P0 + 2 P1 findings — both P0s are contradictions between round 17's own fixes and mechanisms specified in earlier rounds that round 17 didn't cross-check against:

- §10.2.1 (P0) — round 17's outbox-recovery procedure made "`read_latest_tip()` fails → L5 does not start" unconditional, directly contradicting §10.2's `OperatorOverride` sidecar (round 12), which exists specifically to admit startup when the external anchor is unreachable — and the fault-injection matrix's own "sidecar override admits" entry names this exact scenario. Fixed: the recovery-drain procedure now branches explicitly — a normal path (unexpected `read_latest_tip()` failure still blocks startup) and a degraded path (sidecar-admitted: skip reconstruction entirely, since no remote tip is known to reconstruct against; start with an empty ring, admit new appends immediately, raise a persistent degraded-mode alarm, and reconcile the pre-restart backlog later once the anchor becomes reachable again).
- §6.1.1 (P0, cross-file) — the outbox-recovery hard-lag age computation needed a `recorded_utc_ms`-equivalent on every scanned frame, but several control-plane payloads (`FreezeProbeAttemptPayload`, `FreezeEpochWatermarkPayload`, `GenerationBridgePayload`) never had one — making the age check unimplementable for exactly the frame types most likely to matter. Fixed in L5 §6.1.1: `recorded_utc_ms` moved into the **frame header** itself (covered by the same full-frame MAC), present uniformly on every frame regardless of payload type, rather than requiring each payload to carry its own copy.
- §7.3.1 (P1) — `recovered_next_epoch + 1` was an unchecked `std::uint32_t` addition; at `UINT32_MAX` it would silently wrap to `0`, the reserved "never used" sentinel. Fixed: explicit exhaustion check, permanent fence + operator-migration requirement instead of a silent wrap.
- (this file, P1) — "`AbortedPreSend`... no reference to it survives anywhere in this design now" was too absolute — it still appears, correctly, in historical changelog entries documenting the rounds that added and removed it. Reworded to distinguish historical record from normative (currently-required-behavior) references, which is what was actually meant.

**Revision 19** (this revision) fixes round 19's 2 P0 + 2 P1 findings — both P0s are contradictions opened by revision 18's own degraded-path / uniform-timestamp fixes:

- §10.2.1 (P0) — degraded (sidecar) startup started with an **empty** export ring and measured hard-lag only against this process's fresh 256-frame baseline, discarding any pre-restart un-anchored backlog. A crash→restart→new-override cycle could therefore grow unbounded un-exported history until the remote tip became readable. Fixed: durable `LastRemoteAckedTip` (breadcrumb, updated on every remote Ack) is the reconstruction baseline when the remote is unreachable; hard-lag applies to that inherited backlog immediately; a new override cannot reset the budget; if already over hard-lag at admission, degraded mode is **read-only drain** (no new appends) until the remote is reachable and catch-up completes.
- §7.3.1 / L5 §6.1.1 (P0) — every frame header required server-corrected `recorded_utc_ms`, but Phase-B `unknown_time_429` freezes must be persisted **before** a trusted UTC exists and forbid forging one — `append_rate_freeze` could not build a compliant header (deadlock). Fixed: header gains explicit `FrameTimeKind` provenance (`ServerCorrectedUtc` | `UnknownBootstrap`); Unknown frames set `recorded_utc_ms=0`, are MAC-covered, and are **excluded from age/TTL** (frame-count lag still applies); recovery never infers age from Unknown.
- §10.2 `GenerationSeal` content-root (P1) — normative byte list omitted the new header time fields; updated to include `time_kind` + `recorded_utc_ms`.
- L5 §6.1.1.1 (P1) — new mandatory header fields lacked an explicit format-version bump / decoder / compaction-migration rule; fixed in L5 revision 20 (current write version = 3; readers decode prior versions until compaction rewrites a generation).

**Revision 20** (this revision) fixes round 20's P0 + P1 — both are incomplete closures of revision 19's own fixes (provenance allowlist too narrow; tip breadcrumb incomplete across seal path):

- §7.3.1 / §10 / L5 §6.1.1 (P0) — a new freeze episode must Ack `FreezeEpochWatermark` **before** `RateLimitFreeze`, but L5 provenance only authorized `UnknownBootstrap` for the freeze frame itself. With no published clock, `append_freeze_epoch_watermark` could not build a compliant v3 header → Phase-B 429 unpersistable. Fixed: closed `UnknownBootstrap` allowlist covers the whole **freeze episode front-half** (watermark → freeze → same-epoch probe attempts while clock still unpublished); `append_freeze_epoch_watermark(..., FrameTimeKind)` takes provenance in the ABI; `FrameTimeKind` lives in `durable_control_plane.hpp` (this section), not only in L5 prose.
- §10.2 / §10.2.1 / §10.3 (P1) — `LastRemoteAckedTip` was updated on per-frame tip export Ack, but compaction's `export_and_wait_ack(GenerationSeal)` did not advance it before `CURRENT` flip. Remote outage after seal Ack + flip would make degraded scan treat all of gen-N+1 as unexported (false hard-lag / ReadOnlyDrain). Fixed: seal Ack → durable tip `{new_generation, final_seq, final_tip_mac, key_id}` → only then flip `CURRENT`; tip-persist failure aborts the flip.

**Revision 21** (this revision) fixes round 21's 4 P0 + 1 P1 — all are "one branch updated, sibling states/docs still on old semantics":

- §7.3.1 (P0) — "no trustworthy UTC → `source=3`" collapsed 418 permanent fences and long present `Retry-After` into a short unknown-time window. Fixed: **freeze kind first** (§7.3 table), **provenance second**. 418 missing header → `source=2` + `UnknownBootstrap`; 429/418 with present Retry-After → keep kind 0/1 and `conservative_wait_ms = max(parsed, IP-window+pad)` when UTC cannot be written; only 429 missing/malformed without UTC uses `source=3`.
- §7.3.1 / §10 / L5 §6.1.3 (P0) — `FreezeProbeAttempt.cleared=true` already meant "freeze verified lifted," but recovery still waited full `conservative_wait_ms` and compaction retained every `source==3` forever. Fixed: cleared epoch is terminal / not active; recovery skips it; compaction may drop resolved freeze+probe history (watermark retained).
- L5 §6.1.3 (P0) — still said seal Ack → flip `CURRENT` while L4 required tip in between. Synchronized to seal Ack → durable `LastRemoteAckedTip` → `CURRENT`.
- §10 watermark comments/ABI (P0) — "before the epoch it names is used" contradicted `next_freeze_epoch` + allocate-`current` example. Unified: watermark stores the **next allocatable** epoch; using `E` requires Ack of watermark `next=E+1` first.
- §10.2.1 (P1) — per-frame tip updates and seal tip updates could race and regress `(generation, sequence)`. Fixed: single-writer or monotonic CAS; regressions rejected/fenced; export drained/serialized before compaction seal.

**Revision 22** (this revision) fixes round 22's 2 P0 — both are recovery-safety holes left by revision 21's "max uncleared epoch" / overloaded `cleared=true`:

- §7.3.1 / §10 (P0) — selecting only `max(uncleared freeze_epoch)` hid an older permanent (`source=2`) behind a newer timed freeze; clearing the newer epoch forgot the permanent. Fixed: **at most one active episode**; further 429/418 **merge/extend same epoch** (no new watermark); recovery **aggregates** all uncleared epochs fail-closed (`permanent_latch` ∪ max timed waits); compaction must not drop an uncleared permanent because a newer timed exists. Fault-inject: `old-source2 + new-source0/3 + clear-new + restart`.
- §7.3.1 / §10 (P0) — `FreezeProbeAttempt.cleared=true` could clear a permanent fence with no operator artifact. Fixed: terminal clear is `DurableRecordType::FreezeClear` + `FreezeClearKind`; probe/wait clears legal only for non-permanent; `source=2` requires `OperatorAuthorized` with KEK-MAC binding; sink rejects illegal clears; recovery ignores/Corrupt forged probe clears of permanent.

**Revision 23** (this revision) fixes round 23's 2 P0 + 3 P1 — merge/clear/probe/representability/Windows durability holes:

- §7.3.1 / §10 (P0) — same-epoch merge could replace a long `ServerCorrectedUtc` ban with a shorter `UnknownBootstrap` wait ("latest frame wins" fail-open). Fixed: **no time-semantics downgrade**; every merge **copy-forwards** prior `deadline_utc_ms` and takes `max` of both UTC and `conservative_wait_ms`; recovery/compaction fold **all frames of the epoch** (not latest-only).
- §7.3.1 / §10 (P0) — `FreezeClear{ProbeVerified}` had no MAC-covered `/time` proof; a forged clear survived restart/compaction. Fixed: `FreezeTimeProbeProof` bound into `FreezeClearPayload`; sink verifies `serverTime >= bound_deadline` + clock snapshot + response MAC before Ack.
- §7.3.1 (P1) — `release_probe(true)` reset the 8-attempt budget while freeze still active → unbounded `/time`. Fixed: reset **only** after durable `FreezeClear` Ack; any completed probe that does not clear still consumes an ordinal + backoff.
- §7.3 (P1) — `representable` covered only UTC `int64` add; steady deadline overflow undefined. Fixed: `utc_representable` + `steady_representable`; either false → permanent fence.
- §10.3 (P1) — `MoveFileExW(WRITE_THROUGH)` is not POSIX dir-`fsync`. Fixed: honest Windows contract (NTFS + FlushFileBuffers on file and directory handle); claim crash-consistency under that procedure, **not** power-loss equivalence to POSIX until power-cut fault-inject passes.

**Revision 24** (this revision) fixes round 24's 4 P0 + 2 P1 findings, plus a self-caught ordering bug found while re-reading the edited block end-to-end:

- §7.3.1 (P0, self-inflicted) — `FreezeTimeProbeProof::clock_snapshot_seq` referenced a sequence number `ClockOffsetSnapshot` never defined, and `ClockOffsetPublisher` exposed no way to query one — the proof structure was literally unconstructible. Fixed: `seq` added to `ClockOffsetSnapshot`, assigned monotonically by `publish()` itself; the proof's verification scope narrowed to internal consistency only (no historical snapshot registry needed or added — `clock_snapshot_seq`/`clock_offset_ms` are captured inline at proof-construction time, not looked up after the fact).
- §7.3.1 (P0, self-inflicted) — the 8-attempt probe cap could permanently self-lock any timed UTC freeze whose real deadline exceeds the backoff table's ~9-minute span (a multi-hour/day 418 ban would exhaust all 8 attempts from elapsed time alone, long before the deadline could plausibly pass, after which `try_reserve_probe()` refuses forever). Fixed: `release_probe()` now takes the episode's known deadline (as an estimated steady time) and never schedules the next attempt earlier than it — `kMaxTotalAttempts` now bounds genuine retry failures near/after the deadline, not the wait for the deadline itself.
- §7.3.1 / §10 (P0) — `ConservativeWaitCompleted` could clear a dual-deadline episode (UTC + merged unknown-time wait) using only the — potentially much shorter — wait component, bypassing an unresolved, longer UTC ban that §7.3.1's own prose already said must independently be satisfied. Fixed: `ConservativeWaitCompleted` is now illegal whenever the folded episode has an active `deadline_utc_ms != 0`; a dual-deadline episode can only clear via `ProbeVerified` (whose existing dual-deadline binding already proves both components) or `OperatorAuthorized`.
- §6.1.3/L5 (P0, cross-file, self-contradiction) — `is_exchange_final()`'s actual implementation included `Reconciled`, directly contradicting §6.5's later, authoritative statement ("returns true only for Filled, Cancelled, Rejected, and Expired"). Fixed: `Reconciled` removed from the true-set — no legitimate code path in this design ever targets it as a live transition, so excluding it costs nothing correct, while failing closed against the case where a bug or corruption ever did produce it on a still-open order.
- P1 — `FreezeProbeCredit::release_probe()`'s `TimePoint + duration` addition was unchecked, risking UB near `TimePoint::max()`. Fixed: `add_ms_saturating()`, a saturating-clamp helper, replaces the bare addition.
- P1 — `FreezeTimeProbeProof`'s trust boundary and replay binding were left implicit. Fixed: explicitly documented as a local tamper-evidence guarantee (not third-party proof of Binance's identity, since Binance doesn't sign `/time` responses), and the MAC's canonical input extended to bind `freeze_epoch`, `bound_deadline_utc_ms`, a fresh per-attempt `request_nonce`, and `tls_verified_host` — preventing the proof from being replayed across a different episode, attempt, or endpoint than the one it was actually generated for.
- **Self-caught while re-reading the above end-to-end** (not from the review): `release_probe()`'s new call to `add_ms_saturating()` was originally defined as a free function placed AFTER the struct that calls it from an earlier member function — an ordering a compiler would reject. Moved before `FreezeProbeCredit`; the duplicate trailing definition removed.

**Revision 25** fixed round 25's 2 P0 + 2 P1 — dual-deadline wait bypass, probe-budget crash-reset, seq wrap, signing/rotation overflow:

- §7.3.1 / §10 (P0) — dual-deadline episodes forbade `ConservativeWaitCompleted` but `ProbeVerified` only checked `bound_conservative_wait_ms == folded` with no durable wait-completion evidence → UTC `/time` could clear before the conservative wait finished. Fixed: non-terminal `DurableRecordType::FreezeWaitSatisfied` + `append_freeze_wait_satisfied`; dual-deadline `ProbeVerified` **must consume** a matching Acked `WaitSatisfied`; equality-only bind is rejected.
- §7.3.1 (P0) — `FreezeProbeCredit` started empty; first `try_reserve_probe(recovered_epoch)` treated epoch≠0 as "new" and reset `attempts_made`, laundering the 8-cap across crash loops despite `recover_control_plane` outputting attempts. Fixed: mandatory `restore_from_recovery(...)` inject ABI; probes refuse until restored.
- §7.1.2 (P1) — `ClockOffsetSnapshot::seq` had no overflow guard; `UINT32_MAX` wrap broke monotonic/sentinel semantics. Fixed: `publish()` refuses at max, fences, requires operator migration.
- §7.1.2 (P1) — `server_now_ms_pessimistic` / `_for_signing` used bare `+`/`-` (signed overflow UB). Fixed: checked add/sub; failure → Unknown/hold / refuse signing (fail-closed).

**Revision 26** fixed round 26's 2 P0 + 3 P1 — WaitSatisfied forgeability, probe Ack-before-send, backoff/epoch/proof ABI:

- §7.3.1 / §10 (P0) — `FreezeWaitSatisfied` payload/sink equality checks could not prove elapsed wait (merge→immediate append→`ProbeVerified` still early-cleared). Fixed: `DurableRecordType::FreezeWaitArm` arms the wait; sink records `arm_ack_steady` at Arm Ack and **rejects** WaitSatisfied unless its own `(satisfy_ack_steady - arm_ack_steady) >= bound`; Arm without WaitSatisfied is abandoned across process restart (must re-Arm). `ConservativeWaitCompleted` with `wait>0` also requires sink-verified WaitSatisfied (no optional bypass).
- §7.3.1 (P0) — `try_reserve_probe()` was memory-only; `/time` before `FreezeProbeAttempt` Ack + kill -9 could replay the same ordinal forever. Fixed: reservation yields `attempt_ordinal`; **`append_freeze_probe_attempt(...).acked()` is a mandatory precondition of the network send**; Ack advances `attempts_made`; failed Ack → `cancel_reservation()` (no budget burn, no send).
- §7.3.1 (P1) — `restore_from_recovery` zeroed `next_allowed_attempt_steady`, skipping up to 300s backoff. Fixed: persist `not_before_utc_ms` on each attempt; restore re-applies max durable not-before and/or `kBackoffTableMs[attempts_made-1]` from recovery `steady_now`.
- §7.3.1 (P1) — any foreign `freeze_epoch` to `try_reserve_probe` reset attempts. Fixed: epoch switch only via `bind_new_episode_after_durable_create` (after watermark+freeze Ack) or `restore_from_recovery`; reserve accepts **only** `active_epoch`.
- §10 (P1) — proof MAC required `tls_verified_host` but `FreezeTimeProbeProof` lacked the field. Fixed: NUL-terminated `tls_verified_host[64]` on the proof; sink rebuilds MAC from payload; must be non-empty, NUL-terminated, and a member of the bound `EnvironmentBinding` allowlist (the host the probe connection actually TLS-verified — often `base_host()`, or an allowlisted failover).

**Revision 27** fixed round 27's 1 P0 + 2 P1 — durable deadline-aware probe schedule, sink ordinal enforcement, checked not-before arithmetic:

- §7.3.1 / §10 (P0) — `release_probe` deferred to `deadline_steady_estimate` in memory only; durable `not_before_utc_ms` was `now+backoff` alone, and restore ignored folded `deadline_utc_ms` / skipped the UTC gate when `now_utc` was absent → crash during a multi-day 418 burned the 8-cap early (permanent self-lock). Fixed: `not_before_utc_ms = max(checked_now_plus_backoff, folded_deadline_utc_ms)` on every attempt; restore folds `max(attempts.not_before, folded_deadline)`; `now_utc == nullopt` with `not_before > 0` **refuses** FreezeProbeCredit (gate not ignorable); clock re-establish during freeze uses `TimeResyncCredit` (not FreezeProbeAttempt) at most once per boot.
- §10 (P1) — `append_freeze_probe_attempt` now requires sink checks: active epoch, `attempt_ordinal == durable_max+1`, `not_before >= folded deadline` when deadline present; `confirm_attempt_acked` binds Ack `sequence`.
- §7.3.1 (P1) — `now_utc + backoff` uses `checked_add_i64`; unrepresentable → refuse probe / fence (no UB).

**Revision 28** fixed round 28's P0 — freeze-period `TimeResyncCredit` once-per-boot bypass:

- §7.1 / §7.3.1 (P0) — round 27's "once per boot `TimeResyncCredit` while frozen" had **no durable epoch fuse**, did not burn the 8-cap, and reset on every `kill -9` → unbounded `/time` during 418/429 (IP-ban amplifier; contradicts "do not poll long deadlines early"). Fixed: while any uncleared freeze epoch is active, **`TimeResyncCredit` MUST NOT send `/time`**. Clock re-publish while frozen is **only** via `FreezeProbeCredit` + durable `FreezeProbeAttempt` (Ack-before-send, same 8-cap, restored across restart). Missing `now_utc` with `not_before > 0` no longer opens a free lane: a FreezeProbeCredit reservation may proceed solely to re-publish the clock, but **burns a durable ordinal**; after publish, `now_utc >= not_before` is enforced. Exhaustion / failed bootstrap → hold freeze/fence; restart does not refill.

**Revision 29** fixed round 29's P0 — `ClockRepublish` last-ordinal self-lock:

- §7.3.1 / §10 (P0) — `ClockRepublish` burned the 8-cap but was forbidden from `FreezeClear`; `attempts=7 → crash loses clock → ordinal 8 ClockRepublish → serverTime >= deadline` still could not clear → timed episode permanently held with no operator path. Fixed: purpose renamed `ClockRepublishOrVerify`; after Acked `/time` + clock publish, if `serverTime >= folded deadline` and a valid `FreezeTimeProbeProof` (MAC/host/nonce) and dual-deadline `WaitSatisfied` when required, **same response may** `append_freeze_clear{ProbeVerified}`; only hold when deadline still future or proof/WaitSatisfied incomplete. Also: when `conservative_wait_ms > 0` and no WaitSatisfied yet, **refuse all probe reserves** (complete Arm→Satisfy first) so the last ordinal is not wasted before wait evidence exists.

**Revision 30** fixed the `wait_ok` trust gap (sink authoritative wait-gate on `append_freeze_probe_attempt`).

**Revision 31** fixed round 31's P0 — WaitSatisfied reuse after equal-duration rematch — plus self-caught consistency traps in the same change:

- §7.3.1 / §10 (P0) — merge used only `conservative_wait_ms = max(old,new)`; after WaitSatisfied for wait=120, a new unknown 429 with wait=120 left folded duration unchanged so the **old** WaitSatisfied still matched → probe/clear skipped the new wait (survives compaction/recovery). Fixed: every wait-bearing **event** advances durable **`wait_generation`** (even when duration does not increase); Arm / WaitSatisfied / probe wait-gate / clear / recovery / compaction bind that generation; old WaitSatisfied for a prior generation is **rejected**. Must-pass: `unknown 429 wait=120 → WaitSatisfied → new unknown 429 wait=120 → old WaitSatisfied rejected → re-Arm → full wait → Satisfy` before probe/clear.
- §7.3.1 / §10 (P0, self-caught) — writing folded `max(old,new)` wait into every merge frame would make deadline-only merges look wait-bearing and either falsely advance `wait_generation` or contradict the sink's `wait>0 ⇒ gen==prior+1` rule. Fixed: frame `conservative_wait_ms` is **this event's contribution**; folded wait = max across frames; generation advances iff contribution `> 0`.
- §10 (P1, self-caught) — "consume/mark used" WaitSatisfied before clear Ack could strand a generation after a Failed clear. Fixed: WaitSatisfied is **required-present** at successful clear Ack only; Failed clear leaves it reusable for the current generation.

**Revision 32** fixed round 32's P0 — legacy upgrade re-opened equal-wait rematch:

- §10 / §7.3.1 (P0) — additive decode synthesized `wait_generation=1` and treated legacy Arm/Satisfy (`gen=0`/missing) as matching **once**, but legacy logs cannot prove Satisfy occurred after the **last** wait-bearing 429. Reachable: `429 wait → Satisfy → 429 wait → upgrade/restart` → recovery accepted the first Satisfy → skipped re-Arm (fail-open; compaction could solidify it). Fixed: **never** promote legacy Satisfy to current-gen evidence; legacy-wait episodes require a durable **migration**/compaction seal to explicit `wait_generation ≥ 1`, then full `Arm → wait → Satisfy` under that gen (sequence-proven rewrite only when Satisfy.seq > last wait-bearing freeze). Also: while folded wait `> 0`, sink rejects Arm/Satisfy/probe/clear with `wait_generation == 0` so Arm-before-migration cannot recreate a gen-0 match against folded gen `0`.

**Revision 33** fixed round 33's 2 P0s — sequence-proven rewrite Arm gap + compaction vs live write ABI:

- §7.3.1 / §10 (P0) — path-3 only required `Satisfy.seq > last wait-bearing freeze`, so `freeze#1 → Arm → … → freeze#2 → short wait → Satisfy` could rewrite to gen `1` while most of the elapsed time predated the rematch (Satisfy after freeze#2 alone looked “proven”). Fixed: cited `FreezeWaitArm` must exist (same epoch/bound; Satisfy.`arm_frame_seq` binds it) and **both** `Arm.seq` and `Satisfy.seq` must be **strictly greater** than every wait-bearing freeze in the epoch; otherwise drop evidence → `re-Arm → full wait → Satisfy`.
- §10 / L5 §6.1.3 (P0) — compaction prose wrote folded `max(wait_generation)` via the live `append_rate_freeze` ABI, which requires wait-bearing frames to use `prior_folded + 1` — gen `>1` snapshots would Reject (or bypass sink checks). Fixed: dedicated `append_compacted_freeze_snapshot` (+ compacted wait-evidence pair) for compaction into non-`CURRENT` `gen-N+1` only; carries **folded** fields; no `prior+1` rule; no `arm_ack_steady` side effects; live owner must not call it.

**Revision 34** fixed round 34's P0 + 2 P1 — compaction yield tip drift, generation overflow, evidence payload:

- §10.1 / §10.2 / L5 §6.1.3 (P0) — chunk/yield after retain-scan allowed live appends (e.g. new 429) to advance gen-N tip while an already-built N+1 candidate still sealed/flipped from the stale scan → lost N tail + stale WaitSatisfied fail-open on N+1. Fixed: pin `CompactionSourceBaseline{generation, tip_seq, tip_mac, key_id}` at scan start; **re-verify identical tip after every yield resume and immediately before seal / tip-persist / CURRENT flip**; mismatch → abandon N+1 candidate and full rescan (no event loss on N). `GenerationBridge.prev_tip_*` must equal that baseline; recovery accepts N+1 only when archived N's final tip equals `bridge.prev_tip_*`.
- §10 (P1) — `target_generation = source + 1` at `UINT32_MAX` must refuse compaction, permanent fence, offline store migration (no wrap).
- §10 (P1) — `CompactedFreezeWaitEvidence` was enum-only with “one or two frames” optionality. Fixed: single mandatory `CompactedFreezeWaitEvidencePayload` (embedded Arm+Satisfy + source seqs + proof bind); field-by-field encode; full-frame MAC coverage; no alternate layout.

**Revision 35** fixed round 35's P0 + P1 — seal-quiesce volatile buffer + L5 `prev_key_id`:

- §10.1 / §10.2 (P0) — seal quiesce allowed mandatory durable inputs (received 429 → `RateLimitFreeze`, exchange order outcomes) into a **volatile** in-memory replay buffer Acked to the caller before durable write; power-loss after seal Ack / tip persist while `CURRENT=N` then recovery flip to N+1 **lost** the freeze → fail-open re-send. Fixed: **volatile buffers are forbidden** for mandatory durable inputs. **Default (A):** append immediately to live N → tip ≠ baseline → abandon N+1 → reschedule compaction. **Optional (B):** durable seal-side journal (fsynced, outside store root) before any caller `Acked`, with capacity / fail-closed-full / FIFO / recovery-replay contract. Returning “retryable busy” for a received 429/order outcome is **illegal**. Must-pass: `429 during seal → power-loss → recovery still frozen`.
- L5 §6.1.3 (P1) — compaction bridge text omitted `prev_key_id` while L4 binds the 4-tuple — fixed to include `prev_key_id` in write, seal pre-check, and recovery.

**Revision 36** fixed round 36's P0s — post-seal path A vs orphan remote seal + journal drain order:

- §10.1 / §10.2 (P0) — path A (append-to-N + abandon) remained legal **after** `GenerationSeal` export starts / Ack / tip@N+1, which advances N tip off `bridge.prev_tip_*` while remote + `LastRemoteAckedTip` already (or soon) name N+1 → anti-rollback `Corrupt` with no honest “keep CURRENT=N” exit. Fixed: split seal quiesce into **PreSeal** (final tip-check → **before entering** seal export: A or B) and **PostSeal** (export entered / in-flight / Acked → tip persist → CURRENT flip: **B only**; path A illegal). After seal export starts, tip drift is `Corrupt` (not operational abandon). Journal recovery: never replay onto N when a sealed N+1 candidate exists; PostSeal drain is flip-then-replay-onto-N+1.
- §10.1 (P1) — mandatory-input non-durable `Failed`/busy without retained bytes was still a crash-drop class; broadened: caller-visible success requires A-append or B-journal fsync; PostSeal journal-full blocks (never abandon).
- §10 (P1) — `CompactedFreezeWaitEvidencePayload` lacked durable `source_baseline_key_id` (proof had it; frame alone could not bind tip-MAC key lineage). Added field; content-root covers it.

**Revision 37** fixed round 37's P0s — seal export crash window + PostSeal journal deadlock:

- §10.1 / §10.2 (P0) — PostSeal was defined by “entered `export_and_wait_ack`,” but power-loss after remote seal persist and before local Ack left no durable “export started” marker → recovery could treat as PreSeal and journal-replay onto N while remote already holds N+1. Fixed: durable **`SealExportStarted`** breadcrumb (candidate_id, baseline, N+1 final tip, idempotent `request_id`) fsynced **before any seal network write**; recovery seeing it forces PostSeal and queries remote by `request_id`.
- §10.1 (P0) — PostSeal “block when journal full” deadlocked: journal clears only after flip, while in-flight 429/order outcomes still arrive. Fixed: **network quiesce + drain** of all mandatory-capable in-flight requests **before** `SealExportStarted`; **`kPostSealJournalReserve`** soft-cap so PreSeal cannot consume the reserve; PostSeal completions only via owner-driven journal handoff into reserved slots; reserve exhaustion → hard fence (bytes retained, never path A, never RAM-only Ack).
- §10.1 (P1) — seal journal now carries `candidate_id` / `journal_seq` / committed marker; single-writer owner append; ordered recovery scan (not directory mtime).
- §10.1 / L5 (P1) — PostSeal network→owner handoff: fixed-capacity SPSC of **unauthenticated delivery intents** only; authoritative “handled” requires journal fsync on the owner thread (no RAM callback queue as durability).

**Revision 38** fixed round 38 — Unavailable≠absent + seal MAC/breadcrumb completeness (GPT re-filed round-37 items already closed; do not reopen):

- §10.1 / §10.2 (P0) — recovery treated a single `query_seal_by_request_id` → `Unavailable` as “remote never got the seal” and authorized tombstone + PreSeal-abandon while an in-flight export could still land later → orphan remote seal / anti-rollback. Fixed: **Unavailable = not-confirmed-present, not confirmed-absent**; keep `SealExportStarted`, fence, backoff re-query (+ `read_latest` cross-check); abandon only on authenticated negative proof / operator procedure. Must-pass: `SealExportStarted → crash → query Unavailable → remote later Ok` must **not** PreSeal-abandon.
- §10.2.1 / L5 §6.1.3 (P1) — tip-export drain must complete **before** `SealExportStarted` (L5 had it after); seal wait is a **direct** `ExternalAnchorClient` call, not via the paused export-worker ring.
- §10.2 (P1) — normative little-endian field order for `GenerationSeal.mac` / `SealExportStarted.mac`; content-root = ordered hash of full on-disk frame bytes (incl. `time_kind`/`recorded_utc_ms` + payloads incl. `source_baseline_key_id`); recovery Ok MUST byte-match returned seal to `SealExportStarted` bind.
- §10.1 / §10.3 (P1) — Windows flush recipe for `SealExportStarted` / journal committed marker spelled next to CREATE_NEW; incomplete/torn length must not admit PostSeal. (**Round-40:** complete MAC-valid file ⇒ PostSeal — dir-flush is undecidable; see Revision 40.)
- §10.1 (P1) — journal capacity formula: `kSealJournalMaxEntries = kMaxInFlight + kFreezeJournalRoom + kPostSealJournalReserve` with named constants (no magic “floor 16/64” alone).

**Revision 39** fixed round 39's 4 P0s — seal query ABI, request_id durability, journal exactly-once, handoff SPSC:

- §10 (P0) — `query_seal_by_request_id` returned `RecoveryScanStatus` (no `Ok` / no authentic `NotFound`), so recovery branches could not distinguish transport lag from permanent absence. Fixed: standalone **`SealQueryStatus { Found, NotFound, TransportUnavailable, Corrupt }`**; only authenticated **`NotFound`** (or operator-signed cancel) may abandon; `TransportUnavailable` keeps `SealExportStarted` + fence + re-query.
- §10.1 (P0) — `request_id` / `candidate_id` were bare `uint64_t` with no durable high-water / no-reuse contract → restart could reuse an id and flip on a stale remote seal. Fixed: durable **`SealIdWatermark`** breadcrumb; ids reserved with `SealExportStarted` (**round-41:** watermark durable **before** Started — see Revision 41); never reuse; Found path field-by-field breadcrumb + local N+1 bind.
- §10.1 (P0) — journal “append Ack then clear” was not exactly-once across crash between Ack and clear → duplicate apply. Fixed in r39 with a durable applied marker; **round-40 withdraws the two-phase marker** in favor of single-frame `append_seal_journal_apply` (see Revision 40).
- §10.1 (P0) — “I/O threads may push” into one SPSC violated single-producer → tail races / lost 429. Fixed: true SPSC with **one named producer** (`SealCompletionDemux` **or** the owner), **or** per-producer SPSC array; multi-writer into one SPSC forbidden; full → stop read, retain buffer ownership.

**Revision 40** (this revision) fixes round 40's P0s — atomic journal apply + outbox drain before seal + Windows breadcrumb honesty:

- §10.1 (P0) — “business append Ack → later `SealJournalApplied` Ack” still re-applied the payload after crash between the two Acks (e.g. `wait_generation` advanced twice). Fixed: **one** durable write via `append_seal_journal_apply` — `{candidate_id, journal_seq}` + `entry_mac` + business payload in a **single** frame / single Ack (**round-41:** index key excludes `entry_mac`; see Revision 41). Separate post-facto marker is withdrawn.
- §10.1 / §10.2.1 (P0) — PreSeal only drained in-flight tip Acks, leaving up to 256 stale gen-N tuples in `ExportOutboxRing`; after seal tip@N+1 those could still be exported and regress a remote that does not hard-reject lag. Fixed: before `SealExportStarted` **pause producer, empty the ring, finish all tip-export RPCs**; after seal tip@N+1 discard any residual gen-N ring tuples without sending; remote **must** hard-reject lexicographically lagging `(generation, sequence)`.
- §10.1 / §10.3 (P1) — Windows “kill after CREATE_NEW before dir flush → not PostSeal” was undecidable for a complete MAC-valid file. Fixed: **complete + MAC-valid `SealExportStarted` ⇒ PostSeal**; only incomplete/torn length is ignored. Do not pick PreSeal-abandon on an undecidable complete file.

**Revision 41** (this revision) fixes round 41's 3 P0s — journal de-dup key, constructible apply ABI, watermark-before-Started durability:

- §10.1 (P0) — sink indexed by full `SealJournalOriginKey` including `entry_mac`, so “same `{candidate_id,journal_seq}` + different MAC → Corrupt” was unreachable (different MAC looked like a new key → double apply). Fixed: **index key = `{candidate_id, journal_seq}` only**; `entry_mac` is a stored comparison field (equal → idempotent Ack; unequal → `Corrupt`).
- §10.1 / ABI (P0) — `append_seal_journal_apply(const SealJournalApplied&)` could not carry payload bytes (payload only in comments) → unconstructible / unreplayable. Fixed: heap-free **`SealJournalAppliedView`** with `std::span<const std::uint8_t> embedded_payload`; sink copies into one durable on-disk frame synchronously under Ack.
- §10.1 (P0) — “`SealExportStarted` + `SealIdWatermark` in one barrier” across two files is not cross-file atomic: Started durable + watermark not advanced → recovery PostSeal while restart could reallocate the same ids. Fixed: **strict order** — durable advance `SealIdWatermark` (`next_*` past reserved ids) **first**; then `CREATE_NEW`+fsync `SealExportStarted`. Recovery: if Started present, require `watermark.next_* > started.ids` (else fence/`Corrupt` / forward-only repair — **never** reallocate those ids).

**Revision 42** (this revision) fixes round 42's P0s — PreSeal id-before-journal, journal↔apply size/type closed loop, embeddable allowlist:

- §10.1 (P0) — PreSeal in-flight drain allowed Path B before `candidate_id` existed (id assigned only at later step 5) → journal entries unconstructible / RAM ids risk reuse. Fixed: **durable-reserve `candidate_id` + `request_id` (watermark advance) immediately after quiesce, before any Path B / in-flight drain**; `SealExportStarted` later reuses those ids (no second advance). Journaled-but-not-Started recovery = **replay-to-N only**; reserved ids **never reclaimed**.
- §10.1 (P0) — journal Ack admitted payloads larger than `kSealJournalMaxEmbeddedBytes` (4096) while `append_seal_journal_apply` rejects them → PostSeal permanent fence (cannot clear, Path A illegal). Fixed: **journal admission enforces the same size + embeddable-type + schema gates as apply before any journal Ack**; oversize / non-embeddable → hard fence, retain transport bytes, never “handled”.
- §10.1 (P1) — `embedded_type` was any `DurableRecordType` (bridge / override / nested apply possible). Fixed: closed **`is_seal_journal_embeddable_type()`**; journal `record_type` MUST equal apply `embedded_type`; else `Corrupt`.

**Revision 43** (this revision) fixes round 43's P0 — seal-journal time provenance:

- §10.1 (P0) — journal entry metadata omitted `FrameTimeKind` / `recorded_utc_ms` while `append_seal_journal_apply(..., FrameTimeKind)` took provenance from the caller → crash recovery could not restore `UnknownBootstrap` vs `ServerCorrectedUtc`, and replay could re-stamp “now” (breaks Unknown zero-time allowlist + hard-lag age / audit). Fixed: journal entry + **`SealJournalAppliedView`** carry **`time_kind` + `recorded_utc_ms`** (in `entry_mac` domain); admit captures originals; apply/replay **must reuse** them; **removed** the separate `FrameTimeKind` parameter from `append_seal_journal_apply`. Must-pass: `UnknownBootstrap journal → crash → clock published → replay still Unknown+0`; `UTC journal → clock loss → replay still original UTC`.

**Revision 44** fixes round 44's P1 — authenticated journal baseline tip bind:

- §10.1 (P1) — prose listed “baseline tip bind” in journal entry metadata but omitted it from `entry_mac` and from recovery/apply checks → unauthenticated dead field / false security. Fixed (**retain + authenticate**, not delete): explicit `{source_generation, baseline_tip_seq, baseline_tip_mac, baseline_key_id}` in journal entry + `SealJournalAppliedView` + `entry_mac` domain; pinned from `CompactionSourceBaseline` at durable id-reserve; all entries for one `candidate_id` must share one bind; PostSeal/`Found` replay requires bind == `SealExportStarted` / `bridge.prev_*`; PreSeal-abandon / journaled-but-not-Started requires **intra-candidate consistency only** (does **not** require CURRENT tip still equal baseline — tip may have advanced via Path A).

**Revision 45** fixes round 45's P0/P1 — PreSeal B-sticky ordering + Started/Found baseline copy + committed-marker honesty:

- §10.1 (P0) — PreSeal drain allowed Path A after Path B journal Ack → tip advances while earlier observations sit unapplied in the journal → later FIFO replay onto N **inverts observation order** (`wait_generation` / `prior+1` Reject or stale evidence). Fixed: **B-sticky** — once any journal entry is committed for `candidate_id`, Path A is **illegal** until journals are FIFO-applied (abandon→N or PostSeal→N+1). Recovery: unapplied journals + tip ≠ pinned baseline → `Corrupt` (only reachable via B-then-A).
- §10.1 / §10.2 (P1) — `SealExportStarted` write under-specified baseline fields (L5 already required same bind). Fixed: Started MUST copy the **exact** id-reserve pin (must already equal `bridge.prev_*`); forbid re-reading live tip for Started baseline fields.
- §10.1 (P1) — `Found` bind listed “prev tip == baseline” loosely. Fixed: full Started baseline 4-tuple MUST equal `bridge.prev_*` (incl. `prev_generation` / `prev_key_id`) before tip/flip/journal replay.
- §10.1 (P1) — “`entry_mac` plus a durable committed marker” contradicted `HY-SEALJRN-v1` equating them. Fixed: committed marker **is** `entry_mac` under that domain (no second unspecified MAC).
- §10.1 (P2) — `kSealJournalMaxBytes` had no formula after baseline+time metadata growth. Fixed: explicit per-entry fixed metadata + embedded cap product.

**Revision 46** fixes round 46's P0/P1 — mandatory `entry_mac` integrity + packed journal wire layout:

- §10.1 (P0) — `append_seal_journal_apply` treated `entry_mac` as opaque 32B de-dup only (“optionally recomputed”) → old MAC + tampered payload/provenance/baseline could first-apply or idempotent-Ack. Fixed: **every** admit / recovery-load / `append_seal_journal_apply` call **MUST** recompute `HY-SEALJRN-v1` under ambient CURRENT `store_uuid` + `kek_key_id`’s KEK over the view’s MAC-domain fields; mismatch → `Corrupt`, **zero** business side effects. Store frame MAC ≠ journal `entry_mac`.
- §10.1 (P0) — journal admit could Ack without computing `entry_mac`. Fixed: Path B commit **MUST** compute+write `entry_mac` before Ack; KEK/`store_uuid` unavailable → hard fence, never Ack.
- §10.1 (P1) — `kSealJournalFixedMetaBytes=128` was an unverifiable budget guess. Fixed: packed little-endian **SealJournalEntryWire** with exact field widths; constants derived (`kSealJournalFixedMetaBytes=138`); soft-cap checks **both** entry count and `sum(actual_on_disk_bytes)`.
- §10.1 (P1) — `kek_key_id` omitted from journal MAC domain (rotation ambiguity). Fixed: field in wire + View + MAC domain.
- §10.1 (P2) — `journal_seq` / orphan multi-candidate drain under-specified. Fixed: seq starts at 1, strict +1; startup MUST drain **all** journaled-but-not-Started candidates.

**Revision 47** fixes round 47's P0 — journal continuity / Acked-middle silent discard:

- §10.1 (P0) — torn/short/long final `.sj1` treated as “discard like torn log tail” without requiring continuity `1..commit_hw` → Acked-then-damaged/missing middle (e.g. seq=2 deleted while seq=3 remains) could be skipped → permanent loss of 429/freeze/order outcome. Fixed: durable per-candidate **`SealJournalCommitWatermark`** (`highest_committed_journal_seq`); recovery MUST verify contiguous evidence for `1..commit_hw`; only never-published `*.sj1.tmp` may be discarded; final `.sj1` length/MAC/filename anomalies with `seq ≤ commit_hw` → `Corrupt` / hard fence (zero apply, zero CURRENT flip).
- §10.1 (P1) — journal publish used CREATE_NEW on final name (crash mid-write left short finals). Fixed: write `*.sj1.tmp` → fsync → atomic publish to final `.sj1` → parent flush → advance commit watermark → only then Ack.
- §10.1 (P1) — `format_version`/`total_bytes` outside MAC + “long → discard” could erase Acked entries. Fixed: both fields enter `HY-SEALJRN-v1` domain; trailing garbage after a MAC-valid declared span → `Corrupt` (not silent discard).
- §10.1 (P2) — soft-cap `sum(actual_on_disk_bytes)` implied per-admit directory scan. Fixed: owner-held `journal_bytes_used` / `journal_entry_count` rebuilt once at startup, maintained on Acked publish / post-apply clear.

**Revision 48** fixes round 48's P0/P1 — no-replace `.sj1` publish + tombstone lifecycle:

- §10.1 (P0) — “atomic publish/rename” omitted **no-replace**; POSIX `rename` / Windows replace-move could overwrite an Acked final with a stale tmp of the same `(candidate_id, journal_seq)` while `commit_hw` still looked valid. Fixed: publish MUST use no-replace (`renameat2(RENAME_NOREPLACE)` / `linkat`+unlink / Windows non-replacing create); if final exists → MAC-verify + **byte-equal** → idempotent Ack (no rewrite); any difference → `Corrupt`, original unchanged. Forbid §10.3 `MOVEFILE_REPLACE_EXISTING` for `.sj1` finals.
- §10.1 (P1) — required tombstones lacked wire/MAC/GC/capacity contract. Fixed: packed **`SealJournalTombstoneWire`** (includes `entry_mac`); create no-replace; clear order `Applied Ack → tombstone → delete .sj1 → counters`; GC only after `.jhw` cleared; soft-cap counters count **live `.sj1` only**.
- §10.1 (P1) — `.jhw` durable-replace lacked monotonic CAS. Fixed: on-disk `highest_committed_journal_seq` may only stay or increase; regression → `Corrupt`.

**Revision 49** fixes round 49's sibling traps — drain-complete vs GC, counter idempotency, partial-GC resume:

- §10.1 (P0) — “drain complete” required **matching `.jts` ∧ `.jhw` cleared**, but GC deletes `.jts` **after** `.jhw` clear → end state could never satisfy both → permanent refuse-reserve / capacity starvation. Fixed: **drain complete** = continuity OK ∧ all seq Applied ∧ **no** live `.sj1` ∧ **no** `.jhw` ∧ **no** `.jts` for that candidate ∧ counters consistent. Mid-clear may still hold `.jts` while `.jhw` remains.
- §10.1 (P0) — soft-cap mentioned `journal_entry_count` but publish/unlink only adjusted `journal_bytes_used`, and “skip `+=` on idempotent republish” under-counted when final existed but `prev_hw < journal_seq` (publish succeeded, HW not yet advanced). Fixed: both counters; `+=` iff no-replace **created** the final **or** (`prev_hw < journal_seq` on byte-equal path); `-=` / `entry_count -= 1` only after unlink confirmed.
- §10.1 (P1) — crash after `.jhw` delete but before all `.jts` GC left orphans that blocked reserve with no resume rule; `.jts`+`.sj1` dual presence unspecified. Fixed: resume GC delete remaining `.jts` when Applied∧no `.sj1`∧no `.jhw`; dual → verify then unlink `.sj1`; orphan `.jts` without Applied → `Corrupt`; live `.jts` count hard-capped at `kSealJournalMaxEntries` per candidate.

**Revision 50** fixes round 50's Windows IO / crash-window / hot-path traps (GPT residual risks + sibling digs):

- §10.1 / §10.3 (P0) — Windows “hardlink **or** CREATE_NEW copy” left two incompatible crash surfaces (torn copy finals; dirent not flushed before HW). Fixed: **normative preferred path** = fsync(tmp) → `CreateHardLinkW`(final) → parent `FlushFileBuffers` → unlink(tmp); CREATE_NEW copy is **fallback only** when hardlink fails with not-same-volume / not-supported; copy MUST `FlushFileBuffers(final)` + parent flush before any HW advance. Forbid `std::filesystem::rename` / replace-`MoveFileExW` for `.sj1`/`.jts`.
- §10.1 (P0) — counter `-=` after unlink without requiring **parent flush** → NTFS could resurrect `.sj1` after power-loss while soft-cap already decremented (fail-open capacity). Fixed: `-=` both counters **only after** unlink **and** parent directory flush; process-kill consistency claimed; full power-loss equivalence only after matrix entry passes (same honesty as §10.3).
- §10.1 (P1) — no table for the full lifecycle cut points; MAC+byte-equal implied unbounded heap. Fixed: normative **crash-window table** `.tmp→final→parent→.jhw→Ack→Applied→.jts→unlink→GC A/B`; admit/verify uses **preallocated ≤ `kSealJournalMaxEntryBytes`** buffer (no hot-path heap); incomplete `.jts` disposition; `.jhw` may use §10.3 REPLACE (watermark class) with monotonic CAS — never for finals.

**Revision 51** fixes round 51's P0 — GC A deletes `.jhw` while PostSeal Path B can still need `journal_seq`:

- §10.1 (P0) — GC phase A could delete `.jhw` while `SealExportStarted` still present (Started clears only at drain-complete) and handoff intake not proven closed → next Path B has no authoritative `highest_committed_journal_seq` (forbidden to invent from Applied/readdir) → fence / drop / illegal Path A. Fixed: **`.jhw` lifetime covers every state that may call Path B**; **intake-closed barrier** (stop handoff producers, drain+linearize rings empty, zero outstanding mandatory, Path B prohibited) **before** GC A; if barrier unmet → **retain `.jhw`**. `SealExportStarted` present + `.jhw` missing → **fail-closed** for Path B (never guess seq). Must-pass: last Applied → about to GC A → concurrent PostSeal handoff → either `.jhw` still present for seq assign **or** intake already closed so handoff impossible.

**Revision 52** fixes round 52's P0s — early `SealExportStarted` clear + non-linearizable intake-close:

- §10.2 (P0) — crash-window rows cleared `SealExportStarted` right after `journal→N+1` / “drain remaining,” skipping tombstone → intake-close → GC A/B → drain-complete → restart could lose the sole PostSeal gate mid-cleanup. Fixed: **every** recovery/crash cut uses the full chain; **clear Started only at drain-complete** (no `.sj1`/`.jhw`/`.jts`).
- §10.1 (P0) — intake-closed said “stop + empty ring + latch” without a linearizable producer-quiesce ABI → late SPSC `tail` publish after a single empty observation could land a handoff after `.jhw` delete. Fixed: candidate **`SealJournalIntakeClose`** protocol (close epoch, per-producer quiesced ACK, in-flight admit guard, release/acquire, **double-confirm** acquire-drain); Path B latch + GC A only after close proves final; else retain `.jhw` + hard fence.

**Revision 53** fixes round 53's P0/P1 — producer registration set + bounded close:

- §10.1 (P0) — close waited on “every registered producer” but ABI had only a fixed `[8]` array with no `registered_mask` / count / ring bind → true-SPSC waits forever on empty slots, or heuristics miss a late-publish producer. Fixed: **freeze** `registered_producer_mask` + `producer_count` + `ring_id[i]` + `candidate_id` **before PostSeal** (`SealExportStarted`); close waits **only** mask bits; recovery rebuilds the same static topology before demux start; mask change / dup / unknown slot / ring mismatch → hard fence.
- §10.1 (P1) — close said hard-fence on failure but had no `steady_clock` deadline → hung producer ⇒ owner busy-spin forever. Fixed: **`close_deadline_steady`** + bounded poll (`_mm_pause`); timeout → stop reads, retain `.jhw` + bytes, hard fence, **no GC / no clear Started**.

**Revision 54** fixes round 54's sibling traps — post-close apply hole + durable topology + close retry:

- §10.1 / §10.2 (P0) — normative cleanup listed `apply → … → intake-close → GC A`, but intake-close **drains rings into Path B** (new `.sj1` / `commit_hw` advance) → GC A could run while `applied_hw < commit_hw` (orphan journals / silent loss). Fixed: after close **SUCCESS**, owner **MUST** run **post-close apply catch-up** (continuity-checked FIFO apply + tombstone + unlink) until `applied_hw == commit_hw` **before** GC A; GC A while lagging is illegal.
- §10.1 (P0) — topology freeze lived only in RAM → crash + config drift could re-freeze a different mask/ring set than the PostSeal that wrote journals. Fixed: **`SealExportStarted` durably binds** `registered_producer_mask` / `producer_count` / `ring_id[0..kMax)` into its MAC domain; recovery freeze **MUST equal** Started; mismatch → hard fence (do not start demux).
- §10.1 (P1) — non-mask `try_push`/`fetch_add`, `uint8_t` mask width vs `kMax`, and same-process close retry after timeout were underspecified. Fixed: admit from unset bit → **immediate** hard fence; `static_assert(kMaxSealHandoffProducers <= 8)`; timeout retry **MUST** bump `close_epoch` and re-arm deadline (prior ACK for old `E` is void).

**Revision 55** fixes round 55's P0/P1 — §10.3 abbreviated compaction bypass + Started wire ABI:

- §10.3 (P0) — “New generation directory create order” still listed a **simplified** PreSeal/cleanup (`Started` without topology freeze/MAC; `apply/tombstone → intake-closed → GC` without catch-up; recovery “drain journal” shorthand) that contradicted §10.1/§10.2 authoritative chain. Fixed: §10.3 steps **delegate** to §10.1 full PreSeal admission + **single** normative PostSeal cleanup chain; no independent shortcut path. Every `CURRENT` flip path = `freeze → Started(v2) → … → apply → close → catch-up → GC A/B → drain-complete`.
- §10.1 (P1) — `SealExportStarted` was a conceptual struct after r54 topology add, with no `format_version` / `total_bytes` / fixed LE length → r53-length files indistinguishable from truncated v2. Fixed: packed **`SealExportStartedWire`** v2 (`kSealExportStartedWireBytes=234`); MAC domain `HY-SEALSTART-v2` covers version+length+topology; legacy 192B / `HY-SEALSTART-v1` → **fail-closed** (not empty-topology invent); offline migration only.

**Revision 56** fixes round 56's P0 — Started v1→v2 migration deadlock (no-replace vs replace forbid):

- §10 ABI / §10.1 (P0) — offline migrate said “CREATE_NEW / no-replace / atomic replace” onto `seal-export-started` while v2 itself forbids replace and legacy already occupies the name → no-replace always fails; replace illegal; delete-then-CREATE_NEW opens a power-cut window that erases `LegacyStarted` and can look like PreSeal (lose PostSeal gate). Fixed: **companion strategy** — never modify/delete legacy final; `CREATE_NEW` `seal-export-started.v2` + MAC-bound **`SealExportStartedMigrationWire`** (`.mig`) binding `legacy_mac`+`v2_mac`+ids; recovery admits PostSeal from companion **only** when mig complete; every mid-migrate cut stays fail-closed (no PreSeal / no new id reserve).

**Revision 57** fixed round 57's P0/P1 — Started KEK identity + atomic cleanup + closed migrate bind:

- §10 ABI (P0) — `SealExportStartedWire` / `.mig` used `HMAC(KEK,…)` without `kek_key_id` → after rotation recovery/migrate must try-current / try-all (violates L5 §6.1.1.2). Fixed: **`kek_key_id` in v2 (238B) and mig v2 (208B)** MAC domains; legacy 192B verify uses a **single explicit** `legacy_kek_key_id` (operator-supplied into M) — fail-closed, never try-all.
- §10.1 (P0) — “clear L+V+M together” is not atomic on ordinary FS → mid-unlink power-cut could look like `M without L+V → Corrupt` after successful PostSeal. Fixed: durable **`SealStartedCleanupTombstoneWire`** (`.clr`) after journal drain-complete; fixed unlink order; recovery seeing `.clr` **must resume cleanup**, never PreSeal/Corrupt solely for leftover Started names.
- §10 ABI (P1) — L↔V field-bind used ellipsis. Fixed: **closed equality set** (only topology may be new); M binds **SHA-256 full-file digests** of L and V plus trailer MACs + both kek ids.

**Revision 58** fixed round 58's P0 — `.clr` unbound from PostSeal commit:

- §10 ABI / §10.1 (P0) — `.clr` CREATE/CleanupInProgress required only NativeV2/MigratedV2 + empty journal → could authorize clear immediately after Started (pre-seal / pre-flip) → recovery deleted Started, skipped `query_seal_by_request_id`, then remote Found forked the generation chain. Fixed: **`SealStartedCleanupTombstoneWire` v2 (304B)** MAC-binds **PostSealCommittedProof** (`CURRENT == new_generation`, `LastRemoteAckedTip` equal-or-forward covers Started new_*, bridge baseline/ids/content_root, journal drain-complete). Proof fail → **ClrUnauthorized** (query-first PostSeal; never unlink). Draft 176B → never CleanupInProgress. Must-pass: Started + zero journal + wrongful `.clr` + remote Found.

**Revision 59** fixed round 59's P1 — `ClrUnauthorized` self-lock on NotFound abandon:

- §10 ABI / §10.1 (P1) — wrongful `.clr` → ClrUnauthorized → query-first → authenticated `NotFound` + tip==baseline + empty journal may abandon Started, but no legal way to remove the unauthorized `.clr` → name occupied forever → new id reserve refused (single local write/crash → permanent self-lock). Fixed: durable **`SealStartedAbandonWire`** (`.abd`, 192B) **ClrAbandoned** protocol — record abandon proof first, then ordered unlink unauthorized C → Started names → `.abd`; ids never reused; `TransportUnavailable` never writes `.abd`. Must-pass: Started → wrongful C → crash → query NotFound → ClrAbandoned → new candidate reserve.

**Revision 60** fixed round 60's P1 — L5/L4 greenfield Started guard consistency for residual `.abd`:

- §10.1 / §10.2 / L5 §6.1.3 (P1) — reserve already refused `.abd`, but greenfield `CREATE Started` prose/guards could omit residual `.abd` (recovery reordering / dual-doc drift). Fixed: **every** greenfield Started write refuses LegacyStarted/`.v2`/`.mig`/`.clr`/`.abd`; durable `.abd` ⇒ finish AbandonInProgress through ResumeAuthorized + unlink A before any new Started (r60 said “A3”; r62 renames/clarifies). Must-pass: durable `.abd` + watermark reserved → CREATE Started refused.

**Revision 61** fixed round 61's P1 — ClrAbandoned omitted tip-export producer resume and `gen-N+1/` candidate cleanup:

- §10.1 / §10.2.1 (P1) — PreSeal pauses tip-export producer; resume was only after PostSeal `.clr` clear. ClrAbandoned / NotFound abandon never flips and never runs `.clr` → producer stayed paused → `ExportOutboxRing` stall → hard-lag fence. Fixed: **TipExportProducerResume** after durable abandon proof (and Path-A PreSeal-abandon) once CURRENT/tip still bind abandon baseline. (r61 also named a sibling NotFound-without-C resume; r63 withdraws any clear-Started-without-`.abd` side-path and unifies that case under ClrAbandoned A0.)
- §10 ABI / §10.1 (P1) — ClrAbandoned cleared C/Started/`.abd` but left `gen-N+1/` written before Started → next CREATE_NEW fails or disk accumulates / unsafe overwrite. Fixed: phase **GenGone** before unlinking `.abd` — verify bridge/`content_root`/`candidate_id` bind, durable rename-or-delete + parent flush.

**Revision 62** fixed round 62's P1 — A3b use-after-unlink and GenGone false idempotency:

- §10 ABI (P1) — A3 unlinked `.abd` then A3b re-read `A.source_generation`/baseline → crash after A3 made TipExportProducerResume unconstructible. Fixed: phase **ResumeAuthorized** (MAC REPLACE) **while `.abd` still present** — re-verify CURRENT/tip against A, flush (**producer stays paused**), **then** unlink A, **then** TipExportProducerResume (authorized by durable ResumeAuthorized; **never** re-read A after unlink). Deep trap closed: resume-before-unlink would let tip advance past A.baseline while A remains → recovery `tip==baseline` self-lock.
- §10 ABI (P1) — GenGone treated `G` absent as always-idempotent success, swallowing abnormal loss / wrong abandoned target. Fixed: `abandoned/gen-{new_g}-c{id}/` is **no-replace**; existing target must bind-verify; if admitted-Started abandon path and both G and target missing → **Corrupt/IoError**. `G` absent without target is legal only for PreSeal-abandon before Started. Must-pass: rename→power-cut before parent flush; same target name wrong bind → Corrupt; G deleted + target missing after Started → Corrupt.

**Revision 63** fixed round 63's P0 — authenticated-NotFound abandon without C cleared Started before `.abd`:

- §10 ABI / §10.1 (P0) — ClrAbandoned with-C path already required A0 before unlink, but the sibling **no-C** recovery branch still said clear Started → GenGone → TipExportProducerResume. Power-cut after Started unlink / before GenGone loses the unique bind (request/baseline/content_root) → recovery cannot prove/finish N+1 cleanup → false PreSeal / generation fork / late Found undetectable. Fixed: **every** authenticated-NotFound that admits a Started abandon MUST first `CREATE_NEW .abd` (A0). No-C: `present_mask` bit3(C)=0 and `digest_C[32]=0`. Unified flow (with-C and no-C): **A0 → A1 unlink C (no-op if mask.C=0) → A2 unlink Started (M→V→L) → GenGone → ResumeAuthorized (A kept, producer paused) → unlink A → TipExportProducerResume**. Side-path that clears Started without `.abd` on NotFound-without-C is **withdrawn**.
- §10.1 recovery (P0, **narrowed by revision 64**) — r63 treated **no Started + no `.abd` + (G|T)** as **Corrupt/IoError**. That false-positives the legal PreSeal-build crash window (G fsynced before id-reserve / Started). Revision 64 introduces durable **`CompactionCandidateIntent`** so Corrupt applies only when Intent phase proves prior Started publish (**StartedPublished**) or when G/T exists without any Intent.

**Revision 64** fixed round 64's P0 — r63 `no Started + no A + (G|T) → Corrupt` false-positive on legal PreSeal crash:

- §10 ABI / §10.1 / §10.2 / §10.3 (P0) — Compaction writes/fsyncs `gen-N+1/` (G) **before** `candidate_id` reserve / journal / Started / `.abd`. Power-cut after G fsync, before reserve/Started → recovery sees **no Started + no A + G**. Root: `GenerationBridge` has no `candidate_id`; G alone cannot distinguish (1) legal PreSeal-build remnant (never reached Started) from (2) abnormal remnant (Started cleared without A0). Fixed: durable **`CompactionCandidateIntentWire`** (140B; `compaction-candidate-intent`; phases **Building → Reserved → StartedPublished**) CREATE_NEW **before writing G**; REPLACE only for monotonic phase raise; clear only after candidate final cleanup. Revised recovery: **Building|Reserved + no Started + no A** → safe **PreSeal-abandon** (NOT Corrupt); **StartedPublished + no Started + no A** → **Corrupt/IoError** (**narrowed by revision 65** — terminal Finalizing phases are NOT Corrupt); **`.abd` present** → existing ClrAbandoned; every authenticated-NotFound Started abandon still requires A0 `.abd` first. Must-pass: `G fsync → crash (not yet reserve/Started) → restart → cleanup + retry` (not fence); `StartedPublished → Started/A abnormally missing → Corrupt`.

**Revision 65** (this revision) fixes round 65's P0 — PostSeal / ClrAbandoned completion crash window false-Corrupt via StartedPublished Intent:

- §10 ABI / §10.1 / §10.2 / §10.3 (P0) — After r64, PostSeal order was `.clr` completes + Started names gone → clear Intent; ClrAbandoned was unlink `.abd` → clear Intent. Power-cut after Started/A deleted but **before Intent clear** left Intent phase **StartedPublished** + no Started + no A → r64 recovery said **Corrupt/IoError**, mis-fencing a nearly-finished legal cleanup. Fixed: MAC-covered terminal Intent phases **`PostSealFinalizing` (3)** and **`AbandonFinalizing` (4)** on the same **140B** wire (no size bump; phase enum extended; `format_version` stays 1; **no** separate `completion_kind` — phase alone). Phase monotonicity: `Building→Reserved→StartedPublished→PostSealFinalizing` **or** `…→StartedPublished→AbandonFinalizing` (cannot cross PostSeal↔Abandon; cannot jump Building|Reserved→Finalizing). **PostSeal:** after PostSealCommittedProof + `.clr` Authorized durable, **REPLACE Intent→PostSealFinalizing before unlinking any Started name (M/V/L)**; then ordered unlink → TipExportProducerResume → clear Intent. Recovery from PostSealFinalizing: verify CURRENT=N+1, tip/bridge, journal drain, `.clr` progress → finish leftover C/M/V/L → clear Intent (**NOT Corrupt**). **Abandon:** after ResumeAuthorized + GenGone, **while `.abd` still present**, REPLACE Intent→AbandonFinalizing → unlink A → TipExportProducerResume → clear Intent. Recovery from AbandonFinalizing: finish unlink A (if present) / resume / clear Intent (**NOT Corrupt** even if A already gone). **Corrupt narrowed:** Corrupt only if phase==**StartedPublished** (non-terminal) + no Started + no A; keep no Intent+(G|T)→Corrupt; Building|Reserved+no Started+no A→PreSeal-abandon. Leftover Finalizing Intent blocks new build (must finish, never overwrite). Must-pass: power-cut after last `.clr` unlink / last Started gone, before Intent clear → restart converges; power-cut after `.abd` unlink, before Intent clear → restart converges (not fence).

**Revision 66** fixed round 66's P1 — Intent phase-cross / no-skip not crash-verifiable when only REPLACE-overwriteable `phase` survives:

- §10 ABI / §10.1 / §10.2 / §10.3 (P1) — `CompactionCandidateIntent` is a single REPLACE file storing only final `phase`; no `completion_kind`, previous phase, previous MAC, or transition receipt on disk. Constraints “no PostSeal↔Abandon cross” / “no skip” lived only in process memory → after reboot, recovery could accept a buggy MAC-valid `phase=AbandonFinalizing(4)` written after a true `PostSealFinalizing(3)` path (numeric increase) and clear Intent without detecting the illegal cross. Fixed: fixed-size **no-replace** **`CompactionIntentTransitionWire`** (176B; `HY-COMPINTENT-X-v1`; filename `compaction-intent-x-<build_nonce_hex16>-<transition_seq_hex16>.x1`) carrying `from_phase`/`to_phase`/`transition_seq`/`prev_transition_mac` + Intent bind (store_uuid / kek_key_id / baseline 4-tuple / build_nonce / candidate_id / request_id). **Genesis:** CREATE_NEW Intent Building is the genesis (no none→Building `.x1`); chain starts at seq=1 Building→Reserved. **Atomic raise:** persist matching `.x1` (file+parent flush) **before** every Intent phase REPLACE; never REPLACE without prior durable transition. **Recovery:** verify contiguous legal chain `Building→Reserved→StartedPublished→PostSealFinalizing` OR `…→AbandonFinalizing`; missing link / jump / cross / seq gap / prev_mac break / foreign bind → **Corrupt**. Intent.phase must equal last `to_phase`, or one-step lag (`phase == from_phase` after durable `.x1`) → recovery completes REPLACE (not false Corrupt). Intent.phase ahead of last `to_phase` without `.x1` → Corrupt. **GC:** unlink all matching `.x1` **before** Intent unlink at the same final-cleanup moment (PreSeal-abandon / PostSeal / ClrAbandoned). Orphan `.x1` without Intent → Corrupt. **Narrowed claim:** Intent phase monotonic REPLACE alone does **not** prove no-cross after reboot — `.x1` chain does. Preserve Intent 140B phases 0..4; Building|Reserved PreSeal-abandon; terminal Finalizing not false-Corrupt; StartedPublished+missing → Corrupt; ClrAbandoned A0 first; wire sizes Started 238 / mig 208 / clr 304 / abd 192 / Intent 140; transition is a **new** 176B wire + `static_assert`. Must-pass: illegal `PostSealFinalizing→AbandonFinalizing` (or wrong jump) + crash → recovery **Corrupt** (not clear Intent); power-cut after `.x1` durable before Intent REPLACE → recovery completes REPLACE / resumes raise (not false Corrupt / not accept cross); power-cut after `.x1` GC before Intent unlink → GC-lag finish Intent clear (not false Corrupt).


**Revision 67** (this revision) fixes round 67's P0 — multi-file `.x1` GC power-window false-Corrupt:

- §10 ABI / §10.1 / §10.2 / §10.3 (P0) — r66 required recovery to treat any missing link / seq gap in the `.x1` chain as **Corrupt**, while normative terminal GC unlinked all matching `.x1` **before** Intent unlink. Power-cut after deleting e.g. `x1[1]` from `{1,2,3}` with Intent still present therefore hard-fenced **Corrupt** even though GC was legal/spec-compliant. Fixed: fixed-size **no-replace** **`CompactionIntentGcAuthorizedWire`** (204B; `HY-COMPINTENT-GC-v1`; filename `compaction-intent-gc-<build_nonce_hex16>.xgc`) MAC-covered under wire `kek_key_id`, binding at least Intent trailer `intent_mac`, terminal transition MAC (last `.x1` trailer mac, or all-zero iff Building-only with no `.x1`), `build_nonce`, `terminal_disposition` ∈ {PreSealAbandonClear=0, PostSealFinalizingClear=1, AbandonFinalizingClear=2}, `intent_phase_at_auth`, store_uuid, baseline 4-tuple, generations, `candidate_id`/`request_id`. **CREATE `.xgc` only when terminal cleanup already authorizes Intent clear** (PreSeal G cleanup + TipExportProducerResume-if-armed; PostSeal after TipExportProducerResume with Intent PostSealFinalizing; Abandon after TipExportProducerResume with Intent AbandonFinalizing) — Forbidden: CREATE before terminal phase / while Started|A|C still gates cleanup / wrong disposition vs Intent.phase. **Normative GC order:** `CREATE_NEW`+fsync `.xgc` (file+parent) → keep `.xgc` while unlink matching `.x1` one-by-one (parent flush each) → unlink Intent (parent flush) → **unlink `.xgc` last** (parent flush). **Recovery:** valid verified `.xgc` (bind==Intent if Intent present; disposition↔phase; intent_mac / terminal_transition_mac match) → **converge GC** over partial/gapped `.x1` set (finish remaining `.x1` / Intent / `.xgc`) — **NOT Corrupt** for authorized gaps; Intent gone + valid `.xgc` (+ residual `.x1`) → finish clear `.xgc` (and residual `.x1`) only — **NOT Corrupt**; no valid `.xgc` + broken `.x1` chain (gap/missing while Intent live, phase>Building) → still **Corrupt**; forged / wrong-bind / dual `.xgc` / early CREATE → hard fence **Corrupt**. **Supersedes** r66 “GC-lag exception” (phase>Building with no `.x1` finish-clear without authorization artifact): that window is legal **only** under verified `.xgc`. Preserve `.x1` no-replace chain; transition before Intent REPLACE; Intent 140B phases 0..4; transition 176B; Started 238 / mig 208 / clr 304 / abd 192; Terminal Finalizing; ClrAbandoned A0; ResumeAuthorized order; Building|Reserved PreSeal-abandon; hot path control-plane only; fixed packed LE; no hot-path heap; KEK by wire `kek_key_id`; tip equal-or-forward. Must-pass: power-cut before/after each seq unlink, Intent unlink, `.xgc` unlink — converge under `.xgc` or Corrupt only when unauthorized; mid-GC delete `x1[1]` from `{1,2,3}` with Intent+`.xgc` → converge (not false Corrupt); same cut without `.xgc` → Corrupt; forged `.xgc` → Corrupt.

**Revision 68** closes residual normative contradictions that could reintroduce round-67's P0 after the ABI landed:

- §10.1 crash-window table / Recovery step 0 / ClrAbandoned A4 must-pass (P0 residual) — r67 ABI + Mode A/B + Forbidden lists correctly required `.xgc` before any `.x1` unlink, but several **live normative** crash-window rows and the long Recovery paragraph still said TipExportProducerResume → **clear I** (or PreSeal-abandon clear I / GC `.x1` → clear I) **without** prior CREATE `.xgc`, and one disposition row still said bare “`.x1` chain break / seq gap → Corrupt” with **no Mode B carve-out**. An implementer following Recovery/table alone could recreate the multi-file GC false-Corrupt window or illegally clear Intent without GC auth. Fixed: every terminal Intent-clear recovery/crash-window/must-pass path is **CREATE `.xgc` → GC `.x1`* → clear Intent → unlink `.xgc` last** (Building-only PreSeal: `.xgc` with `terminal_transition_mac=0`); Mode-A gap/jump/prev_mac break → Corrupt **only when no valid `.xgc`**; Mode B under verified `.xgc` converges authorized gaps; Reserved PreSeal `.xgc` with zero `terminal_transition_mac` while `.x1` remain → Corrupt; ClrAbandoned A4 / Found NotFound abandon abbreviations include Intent Finalizing + `.xgc`-authorized clear. Wire sizes unchanged (Intent 140 / `.x1` 176 / `.xgc` 204 / Started 238 / mig 208 / clr 304 / abd 192).

**Revision 69** closes the GPT P0 that §10.3's cross-path “unified chain after CURRENT flip” still abbreviated away the mandatory `.xgc` tail:

- §10.3 Cross-consistency / crash-window / must-pass (P0) — live normative prose after step 6 still stated every `CURRENT` flip ends `… → Intent PostSealFinalizing → ordered unlink → clear Intent`, omitting TipExportProducerResume + CREATE `.xgc` → GC `.x1` → clear Intent → unlink `.xgc` last. An implementer following that abbreviated path could unauthorized-delete `.x1`, hit mid-GC gap false-Corrupt, or clear Intent before GC auth. Fixed: §10.3 unified PostSeal chain and Abandon sibling are the **full** `.xgc` order (aligned with §10 ABI Normative GC order / L5 main flow); crash-window rows for §10.3 flip → ordered unlink → crash at `.xgc` CREATE / each `.x1` unlink / Intent unlink / `.xgc` unlink → **Mode B converge** when `.xgc` valid (not Corrupt); Forbidden / must-pass / fault-matrix forbid presenting `ordered unlink → clear Intent` or `resume → clear I` as a complete terminal path. Sibling abbreviated ClrAbandoned crash-window tails (`…→unlink A→resume`) likewise require Intent AbandonFinalizing + TipExportProducerResume + `.xgc`-authorized clear. Wire sizes unchanged (Intent 140 / `.x1` 176 / `.xgc` 204 / Started 238 / mig 208 / clr 304 / abd 192). Hot path: `.xgc` remains control-plane only.

**Revision 70** closed the GPT P0 that early / forged MAC-legal `.xgc` made Mode B undecidable — Mode B previously skipped re-checking the physical cleanup preconditions that CREATE `.xgc` requires:

- §10 ABI / §10.1 / §10.2 / §10.3 (P0) — CREATE `.xgc` preconditions require PostSeal: no Started/C gates left (cleanup finished); Abandon: no `.abd`; respective cleanup complete. Mode B recovery only validated `.xgc`↔Intent MAC/phase/disposition/field bind, then converged GC deleting `.x1`/Intent **without re-checking** those physical cleanup preconditions. Failure path: buggy early write of MAC-legal `.xgc` while `Intent=PostSealFinalizing` but `.clr`/Started still present → restart Mode B deletes Intent/`.x1` → unfinished PostSeal downgraded to “no Intent” → Corrupt/inconsistent. Root: `intent_phase_at_auth=PostSealFinalizing` is **not** proof cleanup complete (that phase is raised **before** Started/C unlink). Fixed: **before Mode B converge**, re-verify **PhysicalCleanupPreconditions(`terminal_disposition`)**:
  - `PreSealAbandonClear`: PreSeal cleanup complete for G/T/journal/Started/A/C as applicable (no admitted Started; A/C absent; TipExportProducerResume-if-armed done).
  - `PostSealFinalizingClear`: PostSealCommittedProof holds (or equivalent already-proven), journal drain-complete, **L/V/M/C all cleared** (no Started/C gates), TipExportProducerResume already executed.
  - `AbandonFinalizingClear`: GenGone + ResumeAuthorized done, **A/Started/C all cleared**, TipExportProducerResume already executed.
  Any `.xgc` coexisting with leftover gates above = **early `.xgc` → Corrupt**; MUST NOT enter Mode B GC. Fault matrix: forged or early-written `.xgc` while PostSeal Started/C still present, or Abandon A still present → restart **hard fence Corrupt**. Trap: Intent already gone + `.xgc` + leftover Started/C/A → Corrupt (not “finish unlink `.xgc` only”); `.xgc` + leftover C only → Corrupt; dual disposition mismatch → Corrupt. Legitimate Mode B preserved: mid-GC after gates truly cleared (partial `.x1` gaps under valid `.xgc`) still converges. L5 “valid `.xgc` ⇒ Mode B” shorthand withdrawn — must also hold PhysicalCleanupPreconditions. Normative GC order unchanged: CREATE `.xgc` → GC `.x1` → clear Intent → unlink `.xgc` last (only when preconditions hold at CREATE **and** at Mode B). Wire sizes unchanged (Intent 140 / `.x1` 176 / `.xgc` 204 / Started 238 / mig 208 / clr 304 / abd 192). Hot path: control-plane only; no hot-path heap.


**Revision 71** (this revision) closes the GPT P0 that r70 Mode B `PhysicalCleanupPreconditions` were non-constructible after a legal mid-GC crash:

- §10 ABI / §10.1 / §10.2 / §10.3 (P0) — r70 required Mode B to re-verify `PostSealCommittedProof` (needs live `.clr` fields) and Abandon `GenGone+ResumeAuthorized` (needs live `.abd`), and treated `TipExportProducerResume` "already executed" as a recovery precondition — but normative order unlinks C/L/V/M (and A) **before** CREATE `.xgc`, then GC `.x1`. Legal path `C deleted → .xgc written → partial .x1 delete → crash` left Mode B needing proof that is gone; 204B `.xgc` stored no durable equivalent; TipExport resume is RAM-only. Fixed by **one coherent design**: grow **`CompactionIntentGcAuthorizedWire` → format_version=2, fixed 316B, MAC domain `HY-COMPINTENT-GC-v2`**, embedding **DurableCleanupAuthEvidence** (no separate TerminalCleanupProof sibling — dual-artifact trap avoided). Evidence binds, at CREATE, at least: PostSeal — `.clr` trailer MAC + Started/bridge/seal identity (`new_final_seq`/`new_final_tip_mac`/`new_key_id`/`content_root`/`started_kind`/`present_mask`) + journal-drain flag; Abandon — `.abd` trailer MAC + `content_root` + GenGone + ResumeAuthorized flags; PreSeal — zeros / Building-only `terminal_transition_mac=0` rules unchanged. **Crash-safe order (capture-then-unlink-then-CREATE):** while C (PostSeal ClrPending; L/V/M already gone) or A (AbandonFinalizing; GenGone+ResumeAuthorized done) is still readable, copy proof into the preallocated control-plane buffer (**same actor critical section; no yield**) → unlink C/A → parent flush → **CREATE `.xgc` binding those digests** (Intent still present) → TipExportProducerResume (**idempotent; NOT a Mode B "already occurred" fact**) → GC `.x1` → clear Intent → unlink `.xgc` last. Recovery CREATE when C/A already gone: reconstruct seal/abandon identity from live CURRENT/tip/bridge/GenerationSeal + Intent; set `gate_absent_at_create=1` and `gate_trailer_mac=0`; still require journal-drain + equal-or-forward tip + bridge bind. **Mode B Step2** verifies DurableCleanupAuthEvidence **from `.xgc`** + live **absence** of leftover gates (Started/C/A/L/V/M) + live tip/CURRENT/bridge consistency with bound proof — **never** by re-reading deleted `.clr`/`.abd`. After durable auth OK, idempotently TipExportProducerResume if pause still armed. Early `.xgc` preserved: leftover Started/C/A with `.xgc` → **Corrupt**; forged/wrong-flag/wrong-disposition proof → **Corrupt**. Legacy v1 204B / `HY-COMPINTENT-GC-v1` → **fail-closed** (not Mode B; offline migration only). Fault matrix must-pass: `C deleted → .xgc written → delete any x1 → crash` and `A deleted → .xgc written → delete any x1 → crash` → Mode B converges without live C/A. Wire sizes: Intent 140 / `.x1` 176 / **`.xgc` 316** / Started 238 / mig 208 / clr 304 / abd 192. Hot path: control-plane only; no hot-path heap.

**Revision 72** (this revision) closes the GPT P0 residual that live normative compaction flows still prescribed `.xgc` **v1/204B** write size/domain after r71 ABI/recovery already required **v2/316B** fail-closed against legacy:

- §10.1 CompactionCandidateIntent before G / TipExportProducerResume / crash-window / Recovery / §10.3 step 6 (P0 residual) — ABI block already defined `CompactionIntentGcAuthorizedWire` as **fixed 316B / `format_version=2` / `HY-COMPINTENT-GC-v2`** with **DurableCleanupAuthEvidence**, and Mode B / recovery already **fail-closed** on legacy v1 204B / `HY-COMPINTENT-GC-v1`. But live §10.1 still said Clear (r67): CREATE `.xgc` (`…204B; HY-COMPINTENT-GC-v1`) then GC `.x1`, and several TipExport / §10.3 / Recovery / crash-window / must-pass Abandon tails still ordered `unlink A → resume → CREATE .xgc` or `TipExportProducerResume → CREATE .xgc` (missing CAPTURE / wrong resume placement / omitting DurableCleanupAuthEvidence). An implementer following those live paths would write a v1/204B `.xgc` (or resume before CREATE) → recovery rejects → **hard self-lock**. Fixed: every live normative PostSeal/Abandon terminal clear path is **CAPTURE DurableCleanupAuthEvidence → unlink C/A → CREATE v2 `.xgc` (316B; `HY-COMPINTENT-GC-v2`) → TipExportProducerResume (idempotent) → GC `.x1` → clear Intent → unlink `.xgc` last**; PreSeal-abandon CREATE uses v2/316B with evidence zeros / Building-only `terminal_transition_mac=0`. **v1/204B** remains only in Historical changelog / Compatibility / “legacy fail-closed” notes — never as the prescribed write size/domain. Sibling: §10.3 step 6 abbreviated `ordered unlink → TipExport → CREATE` restored to full CAPTURE→unlink C→CREATE→TipExport order; crash-window / must-pass Abandon tails aligned; L5 §6.1.3 residual “GC auth wire 204B” synced. Wire sizes preserved: Intent 140 / `.x1` 176 / **`.xgc` 316** / Started 238 / mig 208 / clr 304 / abd 192. Hot path: control-plane only; no hot-path heap; no real orders/credentials.


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
    // Round-11 P1: api4.binance.com and api-gcp.binance.com are INTENTIONALLY
    // excluded from the default production allowlist (see §1 host-failover
    // note) — not an oversight. An operator may extend via a versioned,
    // durably-ACKed allowlist config; silent auto-add of undocumented hosts
    // is forbidden.
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

### 1.1 Exclusive egress IP — fixes round-9 P0 (local trackers cannot defend a shared NAT)

`REQUEST_WEIGHT` and `RAW_REQUESTS` are **IP-scoped** on Binance's side. A single L5/L4 owner thread eliminates *in-process* data races on the local trackers; it does nothing about a second process, a second host behind the same NAT, or any other client sharing the egress IP. Header correction only helps *after* a response — between correction points a co-tenant can exhaust the IP budget while this process still believes it has headroom.

**Hard deployment constraint (not optional hardening):**

1. Exactly one process bound to this design's durable control-plane store (§10) may send Binance REST from a given egress IP. The existing OS-level single-writer lock on the durable store is the enforcement mechanism for "one process" — starting a second instance against the same store fails at construction.
2. That egress IP must not be shared with any other Binance API client (other trading bots, scrapers, manual curl from the same host/NAT). This is an **operator/deployment invariant**, checked at startup via a documented runbook assertion (and, where the platform allows, by binding the process to a dedicated source address / ENI / elastic IP). Violation is out of scope for software recovery — the local trackers will under-count and 429/418 becomes likely.
3. Startup still seeds ORDERS via `GET /api/v3/rateLimit/order` and seeds REQUEST_WEIGHT/RAW_REQUESTS from the first successful response headers before any order-placing send — necessary but not sufficient without (1)+(2).

**Host allowlist / failover (P1, closes round-9/11 gap):** `base_host()` is the primary SNI/Host target. Default production allowlist is exactly `{api.binance.com, api1.binance.com, api2.binance.com, api3.binance.com}`. **`api4.binance.com` and `api-gcp.binance.com` are deliberately excluded by default** — Binance has historically advertised additional regional/GCP endpoints whose availability, certificate SAN sets, and rate-limit IP scoping relative to the classic `api*` set are not part of this design's validated threat/compatibility surface; including them silently would expand the SNI/cert/IP-quota surface without an accepted validation. An operator who has validated a host may add it via a versioned, durably-ACKed allowlist-config frame (same pattern as `EndpointWeightConfigSet`); the factory defaults never auto-expand. Failover to an allowlisted alternate is permitted only when: (a) the primary fails a connect/TLS phase with a classified transport error, (b) the alternate is in *this* environment's allowlist (never cross-environment), (c) SNI/Host/certificate verification use the *alternate's* hostname (never the primary's name against the alternate's cert), and (d) the durable audit records a `TransportFailover` control-plane event. DNS answers that resolve to IPs whose certificates do not match an allowlisted hostname are rejected — no "IP-only" bypass of the allowlist.

## 2. Signing discipline (fixes: query-string/percent-encoding/timestamp gaps)

### 2.1 Query string construction

- Parameters are assembled in a **fixed, deterministic field order** (this spec picks alphabetical-by-key; Binance doesn't require a specific order, but signing must be deterministic and testable).
- **No duplicate parameter names** ever.
- Any parameter value containing characters outside unreserved RFC 3986 (`A-Za-z0-9-._~`) is percent-encoded **before** the signature is computed.
- **Invariant: signed bytes == sent bytes.** The exact byte sequence HMAC-signed by `binance_signer.hpp` must be byte-identical to what's transmitted on the wire. No re-encoding after signing, no signing a "logical" string that differs from the transmitted one — this is the single most common source of real-world Binance signature-mismatch (`-1022`) bugs.

### 2.2 Timestamp and clock sync

- `timestamp` is generated as the **last step** before signing — never cached from earlier in request construction, to minimize clock-skew exposure.
- `recvWindow` is fixed at `transport_policy.hpp::TransportPolicy::recv_window_ms` (currently defaults to 5000ms) and must never exceed Binance's documented hard cap (**60000ms**) — this spec freezes it at 5000ms, full stop, no per-call override.
- **Server-time offset**: maintained as `offset_ms = server_time - (local_send_time + rtt/2)` (classic NTP-style symmetric-latency approximation), computed from `GET /api/v3/time` (§3, public, unauthenticated). Published as an immutable `ClockOffsetSnapshot{offset_ms, error_bound_ms, ...}` via `ClockOffsetPublisher` (§7.1.2 / §9.2) — `std::atomic<std::shared_ptr<const ClockOffsetSnapshot>>` or a mutex-guarded copy; **never** a seqlock over plain fields, and never two independent atomics. Every consumer that cares about direction (fixed-bucket rotation) uses `server_now_ms_pessimistic`; signing uses the point estimate. A resync measured through `rtt > kMaxUsableRttMs` (2s) is discarded outright.
  - **TTL**: offset is considered fresh for 5 minutes after fetch.
  - **Refresh**: attempted proactively before the TTL expires, on a background/init path, not inline with an order-critical request.
  - **Refresh failure**: if refresh fails but the cached offset is still within TTL, keep using it. If TTL has expired and refresh fails, **fail closed** — do not send any signed request with a potentially-stale offset.
  - **`-1021` (`Timestamp for this request is outside of the recvWindow`)**: on receipt, immediately force a resync (re-call `GET /api/v3/time`) before any further signed request. Do **not** blindly retry the same request with the same (mis-synced) clock — this is the same no-blind-retry principle `order_lifecycle.hpp` already enforces for ambiguous submits, applied to clock skew. This rule as stated applies to this spec's own **read-only** requests (§4/§5/§6) — retrying an idempotent GET after a resync has no side-effect to worry about. **`SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` §4.5 defines the POST-path handling: forced resync + route to `Ambiguous` + reconciliation — never a re-POST**, because a `-1021` HTTP response only proves the request left this process, not that Binance's validation layer was where it stopped.
  - **Wall-clock jump detection — fixes round-7 P1 (TTL alone only bounds *elapsed time since fetch*, not whether the local clock has behaved continuously since then).** `offset_ms` is computed from a wall-clock read (`local_send_time`, used to compute `timestamp` for signing) at fetch time — but the 5-minute TTL only guards against the offset going *stale from age*. It says nothing about a **step change to the local wall clock itself** (an NTP correction, a VM pause/resume/migration, an operator manually adjusting system time) happening *within* that TTL window — after such a jump, `offset_ms` is silently wrong from the moment of the jump onward, yet still reports itself as "fresh" until the unrelated 5-minute TTL happens to expire. **Fix**: every offset fetch (and every signed-request timestamp generation, cheaply) records both a wall-clock reading (`std::chrono::system_clock`) and a monotonic reading (`std::chrono::steady_clock`) at the same instant. Freshness is re-verified not just against the TTL but against **consistency between the two clocks since the last fetch**: `|(system_now - system_at_fetch) - (steady_now - steady_at_fetch)| > kMaxClockDriftMs` (a small tolerance, e.g. 1000ms, covering ordinary NTP slewing/scheduling jitter but not a real step change) triggers the same fail-closed path as an expired TTL — force a resync before the next signed request, treating the cached offset as untrustworthy even though its TTL alone hadn't yet expired. This is a cheap, local comparison (two clock reads, one subtraction, no network call) performed at every signed-request preparation, not a periodic background task — catching a jump as soon as the next request would otherwise have used the stale offset.

### 2.3 Signer capability gate — fixes round-11 P1 (Binance now supports HMAC / RSA / Ed25519)

Binance Spot REST currently accepts three API-key types: HMAC-SHA256, RSA, and Ed25519. This codebase's existing `binance_signer.hpp` implements **HMAC-SHA256 only**. Loading an RSA/Ed25519 key into an HMAC-only signer would either fail unpredictably at sign time or (worse) produce a byte sequence that is not a valid signature for that key type, yielding cascading `-1022` / auth failures that look like clock or encoding bugs.

**Fix — explicit capability gate at credential load, not at first sign:**

```cpp
enum class BinanceKeyType : std::uint8_t {
    HmacSha256 = 0,
    Rsa = 1,       // NOT supported by this revision
    Ed25519 = 2,   // NOT supported by this revision
};

// Called once at EnvironmentBinding / client init, before any signed request.
// Detects key type from the provisioned material (PEM headers / key length /
// operator-declared type in the secret store). Returns false (and refuses
// init) for any type other than HmacSha256.
inline bool assert_supported_signer(BinanceKeyType t) noexcept {
    return t == BinanceKeyType::HmacSha256;
}
```

- Init with a non-HMAC key **fails closed** with a clear diagnostic (`UnsupportedSignerKeyType`) — never deferred to the first order.
- A future revision that adds RSA/Ed25519 must introduce an abstract `Signer` interface with per-type implementations and update this gate; until then, claiming support in docs or silently accepting those keys is a defect.
- The wire format in §2.1 (`signature=` hex HMAC) is HMAC-specific; RSA/Ed25519 use different encodings — another reason the gate cannot be "try HMAC and see."

## 3. `GET /api/v3/time` (public, unauthenticated)

Returns `{"serverTime": <ms>}`. Used only for §2.2's offset calculation. No signing required.

## 4. `GET /api/v3/account` (signed, USER_DATA)

Backs `account_truth.hpp`'s `AccountSnapshot`. Same signing pipeline as §2. The existing `check_freshness()` gate in `account_truth.hpp` is unchanged — this section only specifies *how the network fetch that feeds it* is signed, validated, and converted, not the freshness policy itself (that's already spec'd correctly in ADR-019 D6). Round-4 P1-3 flagged the previous "response populates the existing struct" line as insufficient — the following is the concrete contract.

### 4.1 Response schema validation

Required top-level fields: `canTrade` (bool), `balances` (array). Each `balances[]` entry requires `asset` (string), `free` (decimal string), `locked` (decimal string). Missing any required field at either level → the whole fetch is rejected, `AccountSnapshot` is **not** updated (the previous snapshot, if any, remains in place and ages toward `check_freshness()`'s existing staleness gate rather than being replaced with partial or fabricated data).

### 4.2 Asset balance conversion — lossless, no floating point

`AssetBalance::free_ticks`/`locked_ticks` are integers; Binance's `free`/`locked` are decimal strings. Same discipline as §5.2's price/qty conversion: parse the decimal string directly into an integer tick count via a fixed scale (`kBalanceScale = 8` — Binance's `/api/v3/account` response reports balances at up to 8 decimal places for every asset, unlike price/qty which are symbol-specific), using integer arithmetic only. A balance string with more than 8 fractional digits, a malformed decimal (non-digit characters, multiple decimal points, empty string), or a value that overflows `std::int64_t` at that scale → the whole fetch is rejected (same as §4.1 — never truncate a balance silently and proceed with a smaller number than the account actually holds).

### 4.3 Capacity — fixing more assets than `kMaxAssets` cannot silently drop entries

`AccountSnapshot::assets` is a fixed `std::array<AssetBalance, kMaxAssets>`. If `balances[]` contains more entries than `kMaxAssets`, the fetch is rejected outright — **not** truncated to the first `kMaxAssets` entries. Silently dropping assets from a fetch that feeds `validate_pre_trade()`'s exposure/balance checks (`account_truth.hpp`) could hide a real position or balance from risk logic; a capacity overrun must surface as a fetch failure (operator-visible, e.g. via the same escalation path as an exhausted reconciliation, §6.4) rather than a quietly incomplete snapshot. Zero-balance assets (`free == "0.00000000" && locked == "0.00000000"`) may be skipped when constructing `AccountSnapshot` — they don't consume a slot — but any asset with a nonzero `free` or `locked` must be represented, or the fetch fails.

### 4.4 Malformed-response fail-closed summary

Any of §4.1's schema failures, §4.2's conversion failures, or §4.3's capacity overrun collapses to one outcome: **the fetch failed, `AccountSnapshot` is left unchanged.** This mirrors §6's reconciliation-query philosophy (collapse every non-trustworthy case into one outcome, never a partial/best-effort update) — a `/api/v3/account` fetch either produces a complete, correctly-scaled snapshot or it produces nothing, and the existing `check_freshness()` gate is what stops a stale-because-fetch-kept-failing snapshot from being used for a trade decision.

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

    // NEW — fixes round-6 P0 (§6.1.1): `cummulativeQuoteQty` on an order
    // response is a QUOTE-asset amount, scaled by exchangeInfo's
    // quoteAssetPrecision for THIS symbol — a separate, symbol-specific
    // scale from both price_scale and qty_scale, and NOT guaranteed to
    // equal kBalanceScale (8, used for account balances in §4.2 — a
    // different quantity entirely: an account-wide balance figure, not a
    // per-order cumulative fill amount). Conflating the two was round-5's
    // avg_fill_price_ticks bug. Populated from exchangeInfo's
    // `quoteAssetPrecision` field for this symbol.
    std::uint8_t quote_scale{};
};
```

One registry, one refresh operation, one `rules_version` for the whole snapshot (all symbols refreshed atomically together) — not a per-symbol version that could have some symbols stale and others fresh within the same submitted order.

### 5.1.1 Notional scale mismatch — fixes round-5 P0 (a real, currently-shipped bug, not just a spec gap)

`account_truth.hpp::checked_notional()` (existing code) computes `notional = price_ticks * qty_ticks` and its own comment says outright: *"caller must ensure same scale."* That's not achievable as the code stands. `price_ticks` is scaled by `10^price_scale`, `qty_ticks` by `10^qty_scale` — their product is therefore scaled by `10^(price_scale+qty_scale)`, a symbol-dependent value that has no reason to equal any particular number. Yet `validate_pre_trade()` (existing code) compares that raw product directly against **three values from two different scale domains**:

- `rules.min_notional_ticks` / `limits.single_order_notional_cap` / `limits.total_exposure_notional_cap` — configured by whoever populates `SymbolRules`/`ExposureLimits`, with no scale contract at all today.
- `AssetBalance::free_ticks` (the quote asset's balance) — per this spec's own §4.2 fix, populated at a **fixed 8-decimal scale** (`kBalanceScale`), because that's how `/api/v3/account` reports every asset's balance.

For a symbol where `price_scale + qty_scale != 8` (the overwhelming majority — e.g. a symbol with `price_scale=2`, `qty_scale=5` has a native product scale of 7, not 8), the single `notional` variable is being silently compared as if it were in two different units in the same function: correct against `min_notional_ticks`/exposure caps *only if* whoever configured those happened to use the native product scale, and correct against `free_ticks` *only if* the product scale happens to equal 8. Both cannot generally be true at once — this is a real balance/exposure/min-notional misjudgment, not a hypothetical one.

**Fix** — proposed change to `account_truth.hpp`: normalize the raw product to one canonical scale (`kBalanceScale = 8`, chosen because it's what account balances are already fixed to per §4.2) immediately after `checked_notional()`, and use *only* the normalized value for every subsequent comparison:

```cpp
// account_truth.hpp — NEW helper, used by validate_pre_trade() below.
// Fixes round-6 P0: revision 5's version had two real overflow bugs, both
// found by re-checking the arithmetic rather than trusting the earlier
// "checked" label — (a) pow10_i64(delta) was unbounded: for delta >= 19,
// 10^delta exceeds INT64_MAX (~9.22e18) and would silently wrap; (b) the
// ceiling-division idiom `(raw + div - 1) / div` itself overflows when raw
// is within `div` of INT64_MAX, exactly the kind of large-notional input
// this function exists to handle safely, not assume away.
inline bool pow10_i64(std::uint8_t exp, std::int64_t& out) noexcept {
    static constexpr std::int64_t kTable[19] = {
        1LL, 10LL, 100LL, 1'000LL, 10'000LL, 100'000LL, 1'000'000LL,
        10'000'000LL, 100'000'000LL, 1'000'000'000LL, 10'000'000'000LL,
        100'000'000'000LL, 1'000'000'000'000LL, 10'000'000'000'000LL,
        100'000'000'000'000LL, 1'000'000'000'000'000LL,
        10'000'000'000'000'000LL, 100'000'000'000'000'000LL,
        1'000'000'000'000'000'000LL,  // 10^18 — the largest power of 10 that
    };                                 // still fits in a positive int64_t
    if (exp >= 19) return false;  // 10^19 > INT64_MAX — fail closed, never wrap
    out = kTable[exp];
    return true;
}

// Converts a notional at its native product scale (price_scale + qty_scale)
// to kBalanceScale (8), matching AssetBalance::free_ticks/locked_ticks.
// Always rounds UP (ceiling) when scaling down — every caller of this
// function uses the result in a "must not exceed" or "balance must cover"
// comparison, so understating the notional would be the fail-OPEN direction;
// rounding up is the conservative, fail-closed choice for all of them.
//
// CHANGED — fixes round-7 P1: native_scale/target_scale take `int`, not
// `std::uint8_t`. The call site below computes native_scale as
// `rules.price_scale + rules.qty_scale` — both std::uint8_t operands, which
// C++ integer-promotes to `int` for the addition, but a std::uint8_t
// PARAMETER would then silently narrow that `int` back down (e.g. a
// (hypothetically corrupted or malicious) exchangeInfo response with
// price_scale=200, qty_scale=200 sums to 400 as an int, then wraps to 144
// when narrowed to uint8_t — a completely different, wrong scale, with no
// diagnostic). Taking `int` parameters removes the narrowing step entirely;
// the explicit range check below (native_scale/target_scale each in [0,18],
// matching pow10_i64's own bound) is what actually rejects an out-of-range
// input, rather than relying on it happening to wrap into something
// pow10_i64() later rejects on its own.
inline bool rescale_notional_ceil(std::int64_t raw_notional_native_scale,
                                   int native_scale,   // price_scale + qty_scale, as int
                                   int target_scale,   // kBalanceScale
                                   std::int64_t& out) noexcept {
    if (raw_notional_native_scale < 0) return false;
    if (native_scale < 0 || native_scale > 18 || target_scale < 0 || target_scale > 18) {
        return false;  // out-of-range scale — fail closed, never narrow/wrap silently
    }
    if (native_scale == target_scale) { out = raw_notional_native_scale; return true; }
    if (native_scale < target_scale) {
        // Scale UP: exact, no rounding — but checked for overflow, since a
        // large notional at a small native scale can overflow when widened.
        std::uint8_t delta = static_cast<std::uint8_t>(target_scale - native_scale);
        std::int64_t mult = 0;
        if (!pow10_i64(delta, mult)) return false;  // delta too large — fail closed
        if (raw_notional_native_scale != 0 &&
            mult > (std::numeric_limits<std::int64_t>::max() / raw_notional_native_scale)) {
            return false;  // overflow → fail closed, same NotionalOverflow path
        }
        out = raw_notional_native_scale * mult;
        return true;
    }
    // Scale DOWN: quotient/remainder ceiling — fixes round-6 P0's overflow.
    // `raw + div - 1` can overflow when raw is close to INT64_MAX; computing
    // the quotient and remainder separately and only conditionally adding 1
    // to the QUOTIENT (never to raw itself) has no such overflow path, since
    // the quotient is always <= raw.
    std::uint8_t delta = static_cast<std::uint8_t>(native_scale - target_scale);
    std::int64_t div = 0;
    if (!pow10_i64(delta, div)) return false;  // delta too large — fail closed
    std::int64_t q = raw_notional_native_scale / div;
    std::int64_t r = raw_notional_native_scale % div;
    out = (r == 0) ? q : q + 1;  // q < raw_notional_native_scale whenever r != 0
                                  // and div > 1, so q + 1 cannot overflow either
    return true;
}

// Fixes round-7 P1 (companion to the above): price_scale/qty_scale/
// quote_scale are validated to be in [0, 18] at the moment they're parsed
// out of GET /api/v3/exchangeInfo (§5.1's schema validation), not left to
// be caught only here — this function's own [0,18] check is defense in
// depth for a value that should already be well-formed by the time it
// arrives, not the only line of defense against a malformed registry entry.

// validate_pre_trade() — proposed change: after checked_notional() computes
// the raw product, rescale it ONCE and use notional_canonical for every
// remaining comparison (MIN_NOTIONAL, single-order cap, exposure cap,
// balance sufficiency) instead of the raw, ambiguously-scaled product:
//
//   std::int64_t notional_native = 0;
//   if (!checked_notional(price_ticks, qty_ticks, notional_native))
//       return PreTradeCheck::NotionalOverflow;
//   std::int64_t notional = 0;  // now ALWAYS kBalanceScale (8) from here down
//   if (!rescale_notional_ceil(notional_native, rules.price_scale + rules.qty_scale,
//                               kBalanceScale, notional))
//       return PreTradeCheck::NotionalOverflow;
//   // ... rest of the function unchanged, but every comparison against
//   // `notional` is now comparing like-scaled values.
```

This makes an explicit, documented contract out of what was previously an unstated assumption: **`rules.min_notional_ticks`, `ExposureLimits::single_order_notional_cap`, `ExposureLimits::total_exposure_notional_cap`, and `ExposureLimits::current_exposure_notional` must all be populated at `kBalanceScale` (8 decimals), matching account balances** — whoever wires `exchangeInfo`'s `MIN_NOTIONAL`/`NOTIONAL` filter value (itself a decimal string) into `min_notional_ticks` must parse it at scale 8 via the same lossless decimal-string parser §4.2 already specifies for balances, not at the symbol's native product scale. `rules.price_scale`/`qty_scale` must be genuinely populated from a real registry fetch before this function is called — a default-constructed `SymbolRules{}` (`price_scale=0`, `qty_scale=0`, `rules_version=0`) would rescale as if `native_scale=0`, silently multiplying by `10^8`; `validate_pre_trade()`'s existing `SymbolNotTrading`/freshness checks already reject an unpopulated/untrusted `rules` in practice (a real registry entry always has `is_trading` meaningfully set), but this is worth stating explicitly as a precondition rather than leaving it implicit.

### 5.2 Lossless tick→decimal formatting

Converting an integer tick count back to Binance's expected decimal string uses integer arithmetic (division/modulo against `10^price_scale` / `10^qty_scale`), never a floating-point round-trip — matching this codebase's existing `fixed_point.hpp` discipline. No scientific notation, no silently-dropped trailing precision.

### 5.3 Carrying `rules_version` through the call chain — fixes round-5 P0 (TOCTOU: a version-integer comparison at Gate 1 doesn't bind what Gate 8 actually formats)

Revision 4 threaded `expected_rules_version` (an integer) from `validate_pre_trade()` through to `submit_order()`, and round-4's own fix (§2.2 of the L5 spec) made the comparison a real standalone gate, run before `Submitting`. Round 5 found this still isn't sufficient: comparing two version **integers** at Gate 1 says nothing about what data `submit_order()` actually uses to *format* the wire request at Gate 8, several gates later (durable audit writes, CONFIRM, in-flight registration all happen in between). If `submit_order()`'s wire-formatting step does its own, independent, freshly-timed lookup of `symbol` string / `price_scale` / `qty_scale` from whatever the registry's *current* state is at Gate 8 — rather than using the exact data pre-trade validation actually checked against at Gate 1 — an operator refresh landing in that window produces a request formatted from data that was never checked by any gate. Shrinking the window doesn't fix this class of bug (round 3 already established that lesson for `RateLimited`); the fix is to stop taking a second, independent look at all.

**Fix: carry the actual captured `SymbolRules` value through the chain, not just its version number.** `SymbolRules` is a small POD (a handful of `int64_t`/`uint8_t` fields, well under a cacheline) — copying it is cheap, and a copy, once made, cannot be changed out from under its holder by a later refresh. `submit_order()` never performs its own registry lookup for wire formatting; it uses only the `SymbolRules` snapshot it's handed.

```cpp
// account_truth.hpp — NEW: the registry hands out snapshots by value, never
// a pointer/reference into its own storage, so a caller's copy is immune to
// a later refresh no matter how much later that refresh happens.
class SymbolRegistry {
public:
    // Returns a BY-VALUE copy of the current entry for symbol_id, taken
    // under a brief reader lock (refresh is a rare, explicit,
    // operator-triggered, off-hot-path event — a std::shared_mutex reader
    // lock here costs nothing that matters, and is what actually prevents a
    // torn read during a concurrent refresh, which a bare "atomic swap of a
    // multi-field struct" cannot honestly provide without a lock or a real
    // double-buffer scheme; a lock is the simpler correct choice here).
    SymbolRules current_rules(std::uint32_t symbol_id) const noexcept;

    // Operator-triggered refresh (L4 §5 exchangeInfo fetch). Takes the
    // writer lock, replaces every symbol's entry, bumps rules_version for
    // the whole snapshot atomically with respect to current_rules() callers.
    //
    // CHANGED — fixes round-7 P0: takes DurableAuditSink* so the durable
    // write below is atomic with respect to publication, not a separate
    // step callers could reorder or skip. Returns false (refresh REJECTED,
    // in-memory table left completely unchanged) if the durable write
    // fails — see the ordering requirement immediately below.
    // Durable writes go through DurableControlPlaneSink (§10). Caller must
    // be the owner actor thread (§9) — never an operator/CLI thread.
    bool refresh_from_exchange_info(DurableControlPlaneSink& sink /*, exchangeInfo response... */) noexcept;

private:
    mutable std::shared_mutex mu_;
    std::array<SymbolRules, kMaxSymbols> table_{};
};

// validate_pre_trade() signature is UNCHANGED from revision 4 — it already
// took `const SymbolRules& rules` as its first parameter; the fix is in what
// the CALLER does with the rules value it validated against, not in this
// function's own signature.
PreTradeCheck validate_pre_trade(
    const SymbolRules& rules, OrderSide side, std::int64_t price_ticks,
    std::int64_t qty_ticks, const AccountSnapshot& account,
    std::string_view base_asset, std::string_view quote_asset,
    std::int64_t now_ms, const ExposureLimits& limits) noexcept;

// live_submit_orchestrator.hpp — OrchestratorContext carries the CAPTURED
// SNAPSHOT forward (not just its version integer) from pre-trade validation
// to the submit call:
struct OrchestratorContext {
    // ... existing fields ...
    SymbolRules pre_trade_rules_snapshot{};  // CHANGED from round-4's
                                              // `pre_trade_rules_version` —
                                              // the orchestrator sets this to
                                              // registry.current_rules(symbol_id)
                                              // ONCE, immediately before calling
                                              // validate_pre_trade() with it, and
                                              // never re-derives it afterward.
};

// binance_private_rest.hpp (SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md's §2) —
// submit_order() receives the CAPTURED SNAPSHOT itself, not a version to
// re-check against a live lookup:
SubmitResponse submit_order(
    std::string_view client_order_id, std::uint32_t symbol_id,
    OrderSide side, OrderType type,
    std::int64_t price_ticks, std::int64_t qty_ticks,
    const SymbolRules& rules_snapshot) noexcept;  // CHANGED — this is what
                                                    // wire-formatting (§5.2)
                                                    // uses for symbol string /
                                                    // price_scale / qty_scale.
                                                    // submit_order() performs
                                                    // NO registry lookup of its
                                                    // own — there is no code
                                                    // path here that could see
                                                    // newer data than Gate 1
                                                    // already checked.
```

Gate 1 (L5 §2.2/§3) keeps its role as a **fast, cheap pre-check** — comparing `ctx.pre_trade_rules_snapshot.rules_version` against `ctx.submit_port.current_rules_version()` and rejecting early (`StaleRulesVersion`) without running gates 2–7's work if a refresh has obviously already happened — but it is no longer the thing that makes the design *correct*; correctness now comes from `submit_order()` structurally having no way to consult anything other than the exact snapshot Gate 1 already checked. A refresh landing between Gate 1 and Gate 8 changes what `current_rules_version()` would report on a *future* call, but changes nothing about the `SymbolRules` copy already sitting in `ctx.pre_trade_rules_snapshot` — the TOCTOU window is closed by construction, not narrowed.

### 5.3.1 Durably ACK before publish — fixes round-7 P0

Round 6 introduced the `SymbolRegistrySnapshot` durable-record type (`SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` §6.1.2) specifically so `recovery_scan()` could reconstruct the `SymbolRules` a recovered order was actually validated/submitted against — but never specified the *ordering* between writing that durable record and the in-memory registry actually starting to hand out the new `rules_version` via `current_rules()`. Without an explicit order, `refresh_from_exchange_info()` could plausibly swap `table_` (making the new rules_version immediately visible to `validate_pre_trade()`/Gate 1) and only afterward get around to durably recording the `SymbolRegistrySnapshot` frame. An order submitted in that window references a `rules_version` that, from a crash immediately after, has **no corresponding durable snapshot at all** — `recovery_scan()`'s join would find `AuditRecord::rules_version = N` but no `SymbolRegistrySnapshot` frame for `N`, and has no way to reconstruct `price_scale`/`qty_scale`/`quote_scale` for that order — recovery cannot even interpret its own ticks.

```cpp
// SymbolRegistry::refresh_from_exchange_info() — CHANGED signature (fixes
// round-7 P0): takes DurableAuditSink& so the durable write is atomic with
// respect to publication, not a separate step callers could reorder or skip.
    bool refresh_from_exchange_info(DurableControlPlaneSink& sink /*, exchangeInfo response... */) noexcept;

// Mandatory ordering inside the implementation:
// 1. Build the new snapshot (parse exchangeInfo, populate a LOCAL
//    std::array<SymbolRules, kMaxSymbols> — table_ itself is NOT touched yet).
// 2. Durably write the SymbolRegistrySnapshot frame for the new rules_version
//    via DurableControlPlaneSink::append_snapshot(payload, entries) (§10 —
//    FIXES ROUND-8/9 P0: ABI lives in L4, not L5; call is made ONLY on the
//    owner actor thread after a RefreshRegistry task is dequeued — never
//    directly from an operator/CLI thread) and confirm Acked. If NOT Acked:
//    return false immediately. table_ is untouched — the OLD rules_version
//    remains live and fully durable-backed.
// 3. ONLY after step 2's Acked confirmation: take the writer lock, swap
//    table_ to the new snapshot, bump the published rules_version. From this
//    instant on, current_rules() and Gate 1's current_rules_version() begin
//    returning the new version — and by construction, a durable
//    SymbolRegistrySnapshot for it already exists.
```

A caller that observes `refresh_from_exchange_info()` return `false` treats it exactly like any other `AuditWriteNotAcked` condition elsewhere in this design (escalate, do not retry the refresh blindly) — the registry is left in its last-known-good, fully-durable state rather than silently keeping the operator's requested refresh half-applied.

## 6. `GET /api/v3/order` (signed, reconciliation query) — fixes: reconciliation was explicitly out of scope, must not be

ADR-019 D8 requires reconciliation-after-ambiguous to exist before any live POST path is authorized. The original draft excluded it "for later" — the review's first blocking finding. Revision 2 fixed the omission but only defined retry semantics for one specific error code (`-2013`); it left every other failure mode of the query *itself* (5xx, `-1007`, `429`/`418`, DNS/TLS failure, timeout, truncated body, 200-with-schema-mismatch, 200-with-COID-mismatch) undefined. Revision 3 fixes this by collapsing every non-definitive response into one explicit outcome, so there is no code path left that could accidentally treat "the query failed" as "the order doesn't exist."

```
GET /api/v3/order?origClientOrderId={coid}&recvWindow={ms}&symbol={symbol}&timestamp={now}&signature={...}
```

**Fixes round-5 P1**: the previous revision showed this example as `symbol&origClientOrderId&recvWindow&timestamp` — not alphabetical (`o` < `r` < `s` < `t`), directly contradicting §2.1's own "fixed, deterministic, alphabetical-by-key field order" rule, which this section's own text says is "the ONE order both L4 and L5 use." Corrected to genuine alphabetical order: `origClientOrderId`, `recvWindow`, `symbol`, `timestamp`.

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
    bool retry_after_present{false};      // CHANGED (fixes round-7 P0 / round-11 P0)
    std::int64_t retry_after_deadline_ms{0}; // ABSOLUTE UTC wall-clock ms, copied from
                                              // RetryAfterResult::deadline_utc_ms at parse
                                              // time (§7.3). NEVER a steady-clock value.
                                              // Actor scheduling converts at enqueue:
                                              //   steady_deadline = steady_now +
                                              //     max(0, utc_deadline - now_utc_ms)
                                              // Durable freeze persistence uses this UTC
                                              // field (via RateLimitFreezePayload), not a
                                              // re-derived steady number.

    // NEW — fixes round-4 P0: without these, a Found result confirming
    // PARTIALLY_FILLED/FILLED has nowhere to carry the fill data that
    // order_lifecycle.hpp::OrderRecord already has fields for
    // (filled_qty_ticks/avg_fill_price_ticks, same names, same scale as the
    // ones SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md §4.1 added to SubmitResponse).
    // Valid only if outcome == Found AND confirmed_state is PartialFill/Filled;
    // zero otherwise (never Found-but-fill-data-omitted for a filled order).
    std::int64_t filled_qty_ticks{0};      // from response `executedQty`
    std::int64_t avg_fill_price_ticks{0};  // derived — see §6.1.1
};
```

#### 6.1.1 Deriving `avg_fill_price_ticks` — fixes round-6 P0 (dimensional-analysis bug: the round-5 formula silently mixed three different scales)

`GET /api/v3/order` reports `executedQty` (scaled by `qty_scale`) and `cummulativeQuoteQty` (scaled by `quote_scale`, §5.1's new field — **not** `price_scale`, and not `kBalanceScale` either). Revision 5's formula passed only `rules.price_scale` into a generic "scaled division" helper, silently assuming `cummulativeQuoteQty` was already at `price_scale` — it isn't. The correct relationship (worked from first principles, not asserted):

```
avg_fill_price          = cummulativeQuoteQty / executedQty                    (real numbers)
avg_fill_price_ticks    = avg_fill_price × 10^price_scale
                        = (quote_ticks / 10^quote_scale) / (executed_qty_ticks / 10^qty_scale) × 10^price_scale
                        = quote_ticks × 10^(price_scale + qty_scale - quote_scale) / executed_qty_ticks
```

The exponent `price_scale + qty_scale - quote_scale` can be positive, negative, or zero depending on the symbol — there is no single "the scale," which is exactly what the round-5 formula got wrong by hardcoding one. Same integer-only discipline as §5.2 (no floating-point round-trip), computed via a checked, overflow-safe helper that handles the signed exponent and rounds explicitly:

```cpp
// NEW — computes (value * 10^exponent) / divisor as a single checked
// operation, where exponent may be negative (meaning "divide by 10^|exponent|
// first," not "multiply by a fraction" — still pure integer arithmetic).
// A naive value*pow10(exponent) for a POSITIVE exponent risks overflowing
// int64 well before the final division brings it back down (e.g. a large
// quote_ticks value multiplied by 10^6 before dividing by executed_qty_ticks)
// — this is exactly the overflow class §5.1.1's rescale_notional_ceil()
// already had to solve for a similar mul-then-div shape, generalized here to
// a possibly-negative exponent. Implementation uses a 128-bit intermediate
// for the multiply (GCC/Clang: __int128; MSVC: _mul128/_umul128-family
// intrinsics — both compilers this codebase targets provide ONE of these
// paths) so the multiply itself cannot silently wrap before the division
// narrows the result back to int64_t; the final narrowing IS checked
// (returns false, fail-closed, on a result that doesn't fit in int64_t).
// `exponent` is bounded to [-18, 18] (§5.1.1's pow10_i64 bound, reused here
// for the same reason: 10^19 doesn't fit in int64_t) — a scale combination
// outside that range fails closed rather than being computed via any
// unbounded path.
inline bool checked_scaled_mul_div(std::int64_t value,
                                    int exponent,       // may be negative
                                    std::int64_t divisor,
                                    std::int64_t& out) noexcept;

// avg_fill_price_ticks — computed only when executed_qty_ticks > 0.
if (executed_qty_ticks == 0) {
    // NEW == still fully unfilled; confirmed_state won't be PartialFill/Filled
    // in this case, so filled_qty_ticks/avg_fill_price_ticks both stay 0 —
    // never divide by zero, never fabricate a price for a fill that didn't happen.
    result.filled_qty_ticks = 0;
    result.avg_fill_price_ticks = 0;
} else {
    result.filled_qty_ticks = executed_qty_ticks;
    int exponent = static_cast<int>(rules.price_scale) + static_cast<int>(rules.qty_scale)
                 - static_cast<int>(rules.quote_scale);
    if (!checked_scaled_mul_div(cumulative_quote_qty_ticks, exponent,
                                 executed_qty_ticks, result.avg_fill_price_ticks)) {
        // Overflow, or a scale combination too extreme to represent — fail
        // closed exactly like any other reconciliation trust failure: this
        // becomes Inconclusive, never a fabricated/wrapped price.
        return ReconcileQueryResult{ReconcileQueryOutcome::Inconclusive};
    }
}
```

`apply_confirmed_state()` (§6.2) copies both fields directly onto `OrderRecord::filled_qty_ticks`/`avg_fill_price_ticks` — the same fields `SubmitResponse` already populates for a fill discovered via the direct submit response, so a fill discovered via reconciliation is represented identically to one discovered via the original POST response. `SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` §4.2's direct-POST-response fill derivation uses this exact same `checked_scaled_mul_div()` function — one formula, one implementation, for both code paths that compute an average fill price.

**Every one of the following collapses to `Inconclusive`** — none of them, individually or repeated, may ever be read as "the order does not exist" or as any other definitive conclusion:

- `-2013 Order does not exist` (the specific case revision 2 handled) — Binance's Memory→Database visibility lag means this can be transiently wrong even when the order was accepted moments earlier.
- HTTP 5xx (including `-1007` matching-engine timeout).
- HTTP `429`/`418` (rate limited — `retry_after_present`/`retry_after_deadline_ms` populated via §7.3's `parse_retry_after()`).
- DNS failure, connection reset, TLS handshake failure, connect/read timeout.
- Truncated/incomplete response body.
- HTTP 200 but the body fails §6.1.2's full schema/field-match validation below (not just a `clientOrderId` mismatch).

#### 6.1.2 `Found` requires a full schema + field-match contract — fixes round-6 P0 (round 5's fix named the wrong field, which would have made `Found` unreachable)

**Round 5's fix required the *response* to contain `origClientOrderId` as a schema field and checked it for a match.** That field name is a `GET /api/v3/order` **request parameter** (used to look an order up by client-supplied ID, per the query string in §6) — it does not appear in Binance's response body at all. The response identifies the order via a field literally named `clientOrderId`. A schema-validation step that requires a nonexistent field to be present would classify **every legitimate response** as schema-invalid → `Inconclusive` → three attempts exhausted → `EscalateToOperator`, unconditionally, for every reconciliation this design would ever attempt. This is corrected below, along with fixing the cross-check to actually be checkable, per round 6's second finding: `query_order()`'s ABI (`SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` §2) only ever took `symbol` + the client order ID — it had no side/type/price/qty/rules-scale to check the response fields *against*, making round 5's "cross-check against the caller-supplied expectation" unimplementable as specified, not just under-specified.

**Fix — `query_order()` takes an immutable `OrderExpectation`, captured once at submit time, never re-derived:**

```cpp
// account_truth.hpp or order_lifecycle.hpp — NEW. Everything query_order()
// needs to validate a response against, captured at the moment this order
// was actually submitted (mirrors RecoveredOrderRecord's fields exactly —
// same data, same reason: a query must be checked against what THIS
// process actually believes it sent, never re-derived from a live lookup
// that could have moved on, per §5.3's TOCTOU-closing pattern).
struct OrderExpectation {
    ClientOrderId client_order_id{};
    std::uint32_t symbol_id{0};
    OrderSide side{OrderSide::Buy};
    OrderType order_type{OrderType::Limit};
    std::int64_t intended_price_ticks{0};
    std::int64_t intended_qty_ticks{0};
    SymbolRules rules_snapshot_at_submit{};  // needed for price_scale/qty_scale/
                                               // quote_scale to interpret the response

    // Built directly from an OrderRecord (the live, non-recovery path) or a
    // RecoveredOrderRecord (§6.1.2 of SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md's
    // §6.1 — post-restart path) — both now carry every field this needs.
    static OrderExpectation from(const OrderRecord& rec) noexcept;
    static OrderExpectation from(const RecoveredOrderRecord& rec) noexcept;
};
```

**This requires `OrderRecord` itself (`order_lifecycle.hpp`, existing struct) to gain the same two fields round 5 already added to `RecoveredOrderRecord`** — `side`/`order_type` were never on `OrderRecord` in the first place, and neither was a captured `rules_snapshot_at_submit`; without them, the *live* (non-recovery) reconciliation path has exactly the same problem the recovery path had, just not yet noticed because no revision had tried to actually build an `OrderExpectation` from `OrderRecord` until now:

```cpp
// order_lifecycle.hpp — proposed additions to the existing OrderRecord:
struct OrderRecord {
    // ... existing fields unchanged (client_order_id, exchange_order_id,
    // symbol_id, state, submit_timestamp_ms, last_update_ms,
    // intended_price_ticks, intended_qty_ticks, filled_qty_ticks,
    // avg_fill_price_ticks, query_attempts) ...

    OrderSide side{OrderSide::Buy};             // NEW — set once at submit,
                                                  // never mutated afterward
    OrderType order_type{OrderType::Limit};     // NEW — same
    SymbolRules rules_snapshot_at_submit{};     // NEW — the L4 §5.3 snapshot
                                                  // this order was actually
                                                  // validated/formatted against;
                                                  // set once at submit, immutable
};
```

```cpp

// SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md §2 — query_order()'s CHANGED signature:
ReconcileQueryResult query_order(
    const OrderExpectation& expected) noexcept;  // CHANGED (fixes round-7 P1) —
                                                    // was (symbol, orig_client_order_id),
                                                    // round 6 made it (symbol, expected).
                                                    // Dropping the separate `symbol`
                                                    // parameter entirely: it was an
                                                    // independent value that could
                                                    // drift from expected.rules_snapshot_
                                                    // at_submit.symbol (the SAME symbol
                                                    // this order was actually validated
                                                    // and formatted against, §5.3) —
                                                    // two representations of "which
                                                    // symbol" that a caller could
                                                    // accidentally pass inconsistently.
                                                    // The query string's `symbol=`
                                                    // parameter (§6) is now built from
                                                    // expected.rules_snapshot_at_submit.symbol
                                                    // exclusively — one source of truth,
                                                    // matching §5.3's TOCTOU-closing
                                                    // "carry the snapshot, don't re-derive"
                                                    // pattern applied here too.
```

1. **Schema**: required response fields — `symbol`, `orderId`, `clientOrderId`, `price`, `origQty`, `executedQty`, `cummulativeQuoteQty`, `status`, `side`, `type`, `timeInForce`. Missing any → `Inconclusive` (schema-invalid, per the list above).
2. **Field-match** against `expected` (not a live lookup — the captured snapshot): `clientOrderId == expected.client_order_id`, `symbol == expected.rules_snapshot_at_submit.symbol`, `side == expected.side`, `type == expected.order_type`, `timeInForce == "GTC"` (**fixes round-7 P1** — the schema list already required this field's presence, but no earlier revision actually checked its value; this codebase only ever sends `GTC`, L5 §1, so any other value is not evidence of *this* order, exactly like a mismatched symbol/side), `price` (compared as the same scaled-integer representation, using `expected.rules_snapshot_at_submit.price_scale`) `== expected.intended_price_ticks`, `origQty` (using `.qty_scale`) `== expected.intended_qty_ticks`. Any mismatch → `Inconclusive`, exactly like a schema failure — a response that identifies itself by `clientOrderId` but disagrees on symbol/side/price/qty/timeInForce is not trustworthy evidence about *this* order, and must never be treated as `Found`.

Only after both steps pass does `status` select `confirmed_state`, and `executedQty`/`cummulativeQuoteQty` (interpreted using `expected.rules_snapshot_at_submit`'s scales) populate `filled_qty_ticks`/`avg_fill_price_ticks` (§6.1.1).

### 6.2 Retry loop lives in the orchestrator, not inside `query_order()` (fixes round-3 P0: nothing defined who increments/persists `query_attempts`)

`query_order()` (the client method, `SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` §2) performs **exactly one** query attempt and returns — it does not loop internally. The retry loop, the `query_attempts` increment, and the durable persistence of each attempt all live in the orchestrator's reconciliation-handling code, because `order_lifecycle.hpp::determine_reconcile_action()` reads `OrderRecord::query_attempts` to decide `QueryOrder` vs. `EscalateToOperator` — if the count only lived inside a client-internal loop, that field would stay at 0 forever and the state machine could never naturally escalate.

```cpp
// Orchestrator-level reconciliation TASK (proposed, not yet implemented).
// Fixes round-4 P0: every append_durable() call's return value is now
// checked — a write that isn't confirmed Acked must not be treated as if
// it happened, matching SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md §6.2's
// AuditWriteNotAcked discipline for the submit path.
//
// FIXES ROUND-8 P0 (structure): this is ONE actor-queue invocation (§9.1),
// not a while(true) that retries inline. Every terminal path of a single
// invocation returns; Inconclusive paths schedule a future re-enqueue and
// return. A while(true) that "schedules then continues" is exactly the
// tight-loop bug round 8 found — remove the loop shape so the bug class
// cannot be reintroduced by forgetting a return.
void reconcile_ambiguous_order(OrderRecord& rec, BinancePrivateRestClient& client,
                                DurableAuditSink& audit,
                                RequestWeightTrackerSet& weight,
                                RawRequestsTrackerSet& raw,
                                const EndpointWeightConfig& weight_cfg) noexcept {
    // R-10 (Owner decision 2026-09-29): the decision also reads the first-Ambiguous anchors and both clocks --
    //   determine_reconcile_action(rec, now_mono_ms, now_wall_ms, quarantine_policy)  (order_lifecycle.hpp)
    auto action = determine_reconcile_action(rec);  // reads rec.query_attempts
    if (action == ReconcileAction::NoAction) {
        return;  // not Ambiguous — wrong task enqueued; do nothing
    }
    if (action == ReconcileAction::EscalateToOperator) {
        rec.transition_to(OrderState::EscalatedToOperator);
        if (!audit.append_durable(/* OrderEscalated */).acked()) {
            // The state transition already happened in memory and cannot be
            // silently undone (the order genuinely IS escalated — we ran out
            // of attempts). What's unconfirmed is only the DURABLE RECORD of
            // that fact. Fail closed on the recovery side instead: escalate
            // to operator via the same out-of-band channel used for any
            // audit-unavailable condition (ADR-019 D10), since a crash right
            // now would otherwise lose the escalation record entirely.
            escalate_via_out_of_band_channel(rec, "escalation audit write not acked");
            return;  // Acked failed → sink is fencing anyway; do NOT move ledgers
        }
        // Round-10 P1: on a durably-ACKed escalation the order moves from
        // InFlightRegistry to the EscalatedLedger (L5 §6.5) — capacity for new
        // submissions is freed; COID-reuse protection and operator-resolution
        // tracking are the ledger's job from here on.
        move_to_escalated_ledger(rec);
        return;
    }
    // action == ReconcileAction::QueryOrder
    //
    // ORDERING (fixes round-7 P1 + round-9 P0):
    //   1) reserve weight (+ RAW_REQUESTS) FIRST
    //   2) then increment + durably persist query_attempts
    //   3) then issue the GET
    //
    // Round 7 correctly moved persist-before-GET (crash-loop must not send
    // unbounded real queries while the reconstructed count never advances).
    // Round 8 then inserted weight reservation *after* persist — so a freeze
    // or exhausted REQUEST_WEIGHT budget still consumed an attempt slot and
    // could escalate every Ambiguous order to a human without a single GET
    // ever leaving the process. Round 9 restores the safety of persist-before-
    // GET while refusing to charge query_attempts for a send that never became
    // eligible: reservation failure schedules a retry and returns with
    // query_attempts UNCHANGED.
    if (!try_reserve_weight_only(weight, raw, PrivateRestEndpoint::GetOrder, weight_cfg,
                                  /*server_now_ms=*/current_server_now_ms())) {
        schedule_reconcile_retry_at(rec, scheduled_backoff_deadline_ms(rec.query_attempts));
        return;  // no attempt consumed — budget/freeze, not a real query
    }

    rec.query_attempts++;
    auto attempt_ack = audit.append_durable(/* AuditEventType::OrderReconciliationAttempted,
                                 includes rec.query_attempts — NOT result.outcome,
                                 since the query hasn't run yet at this point */);
    if (!attempt_ack.acked()) {
        // Roll back the in-memory increment — this attempt is not
        // happening at all (never sent). Weight was reserved above; it is
        // NOT rolled back (same post-decision rule as §7.2 — we may have
        // already decided to send; the durable write failure is a local
        // fault, and over-counting weight is fail-closed).
        rec.query_attempts--;
        rec.transition_to(OrderState::EscalatedToOperator);
        escalate_via_out_of_band_channel(rec, "reconciliation-attempt audit write not acked");
        return;
    }

    // CHANGED (fixes round-6 P0 / round-7 P1) — full OrderExpectation;
    // symbol from snapshot only.
    auto result = client.query_order(OrderExpectation::from(rec));

    if (result.outcome == ReconcileQueryOutcome::Found) {
        // §6.5 below — maps to the real confirmed state, not a blanket terminal.
        // apply_confirmed_state() itself must check its own append_durable()
        // return value (see below) before returning to this task's caller.
        // If the confirmed state is still live (Accepted/PartialFill/
        // CancelRequested), §6.6 schedules an open-order status poll —
        // do NOT issue another immediate query here.
        apply_confirmed_state(rec, result, audit);
        return;
    }
    // Inconclusive: MUST leave this invocation. determine_reconcile_action()
    // will see the incremented query_attempts on the *next* scheduled run
    // and may return EscalateToOperator.
    //
    // FIXES ROUND-8 P0: an earlier revision called schedule_reconcile_retry_at()
    // and then fell through the bottom of while(true) with neither return nor
    // break — the next iteration immediately re-issued GET /api/v3/order,
    // completely ignoring both the fixed backoff schedule and any Retry-After
    // freeze carried on the Inconclusive result. Three attempts burned in a
    // tight loop; under a real 429 this also hammers Binance during an active
    // ban. schedule_* enqueues work for a FUTURE actor tick (§9.1); it is not
    // a sleep, and it is not a loop-control primitive — the only correct
    // continuation after scheduling is to return from this task.
    //
    // FIXES ROUND-8 P0 (companion): §6.3 already required
    // max(backoff, retry_after) when a Retry-After deadline is present; the
    // previous ternary picked ONE of the two and could schedule an earlier
    // retry than the freeze allowed (or ignore a longer backoff). Always take
    // the later of the two absolute deadlines.
    {
        const std::int64_t backoff_deadline =
            scheduled_backoff_deadline_ms(rec.query_attempts);
        const std::int64_t freeze_deadline = result.retry_after_present
            ? result.retry_after_deadline_ms
            : backoff_deadline;
        const std::int64_t retry_at =
            (freeze_deadline > backoff_deadline) ? freeze_deadline
                                                 : backoff_deadline;
        schedule_reconcile_retry_at(rec, retry_at);
    }
    return;  // CRITICAL — do not loop; the actor will re-enter at retry_at
}
```

`apply_confirmed_state()` (§6.5) durably records the resolved state (e.g. `OrderReconciled` / the specific terminal event) via `append_durable()` before releasing `InFlightRegistry`; that call's return value is checked with the same fail-closed pattern — an unconfirmed write there must not release the in-flight slot, since a crash immediately after would then have no durable record that the order was ever resolved.

**Canonical order-event extension table:** all event values below are appended once in `audit_trail.hpp`, range-checked on decode and encoded field-by-field; no caller overloads `detail_code`/`detail_msg` for recovery state. `OrderSubmitPrepared` carries resulting `Submitting`; `OrderClockDesyncObserved` carries no lifecycle transition; `OrderReconciliationAttempted` carries `query_attempts`; `OrderOpenStatusPolled` carries `open_poll_failures`; `OrderOpenPollFailuresReset` carries `{open_poll_failures=0, reset_after_seq}`; `OrderOperatorResolved` carries the terminal resulting state/fill; `OrderSubmitAborted` is permitted only before any Prepared frame. `AuditRecord` gains explicit `event_sequence_ref` and `last_poll_completed_utc_ms` fields for these contracts. Replay validates event-type↔state against this table; an unknown combination is `Corrupt`.

**Recovery**: `DurableAuditSink::recovery_scan()` must reconstruct `query_attempts` from the durable log of `OrderReconciliationAttempted` events for each in-flight order, not just re-populate `InFlightRegistry` membership — a process restart mid-reconciliation must resume from the correct attempt count, not reset to 0 (which could either retry beyond the intended cap across restarts, or, if miscounted the other way, escalate too early). See `SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` §6.1's `RecoveredOrderRecord` for the concrete signature carrying this (fixes round-4 P0-2 — the interface previously had no way to return anything beyond bare `InFlightRegistry` membership).

### 6.3 Rate-limit interaction

If an attempt's `Inconclusive` was caused by `429`/`418`, `ReconcileQueryResult::retry_after_present`/`retry_after_deadline_ms` carry §7.3's `deadline_utc_ms` **directly** (fixes round-7 P0 duration gap; round-11 P0 clarifies the domain is UTC, not steady). §6.2's task converts both the fixed backoff and the UTC retry-after into steady deadlines at enqueue time and schedules `max(backoff_steady, retry_after_steady)` — it must never re-send (and must `return` after scheduling) while a rate-limit freeze (§7 / §7.3.1) is in effect, even if the fixed backoff schedule would otherwise allow it sooner, and per §7.3's fix, a genuinely long `Retry-After` (hours to days, per Binance's documented 418 ban durations) is honored in full, never clamped down to something that would resend into an active ban.

### 6.4 Exhaustion

If `determine_reconcile_action()` returns `EscalateToOperator`, §6.2's loop transitions to `EscalatedToOperator` — **this is "we don't know, a human decides," never "the order was confirmed absent."** **R-10 (Owner decision 2026-09-29, implemented in-process) replaced the former trigger (`query_attempts` reached `kMaxQueryAttempts` = 3, all `Inconclusive`, i.e. quarantine about a second in)**: the order escalates when at least 5 *sent* queries came back without an answer **and** at least 5000 ms have passed since it first became `Ambiguous`, **or** at least 15000 ms have passed since then whatever the count. Elapsed time is the larger of a monotonic and a wall-clock reading (the wall clock can only bring escalation forward), and a query that never left the process (`QueryOutcome::NotSent`: local rate-limit refusal, stale signing clock, missing credentials) is not counted. The decision function needs a correctly-incremented `query_attempts` (sent queries only, saturating) and the first-Ambiguous anchors `OrderRecord::unknown_since_mono_ms/_wall_ms`; **the wall anchor survives a restart** (limitation L-30, wall-clock part closed): recovery seeds it from the MAC-verified header time of the durable frame that first made the order uncertain (`OrderRecoveryCheckpoint::unknown_since_utc_ms`, no on-disk change), so the unresolved time keeps accumulating across restarts and crash loops. Because that reading also counts the time the process was down, it may trigger an escalation only after this process has sent the recovered order at least one query (every recovered order gets one authoritative look first); the monotonic anchor still restarts with the process. See `docs/SPEC_INVARIANTS.md`'s `InFlightRegistry` entry.

### 6.5 Transition on `Found` — fixes round-3 P0 (a real safety bug, not just missing detail)

Round 2 said a `Found` result transitions the record to `Reconciled` — but `Reconciled` is one of `order_lifecycle.hpp::is_terminal()`'s terminal states. If Binance's confirmed status is `NEW` or `PARTIALLY_FILLED`, the order is **still open on the exchange** — marking it terminal would make the system stop tracking a live position, which is a genuine correctness/safety bug, not a modeling nicety.

Fix: `order_lifecycle.hpp::validate_transition()`'s `Ambiguous` case is extended (proposed change to existing code) to allow transitioning into whichever state matches Binance's confirmed status, not force everything through `Reconciled`:

```cpp
// Proposed change to order_lifecycle.hpp::validate_transition()'s Ambiguous case:
case OrderState::Ambiguous:
    if (to == OrderState::Accepted ||        // confirmed NEW — still live, resting
        to == OrderState::PartialFill ||     // confirmed PARTIALLY_FILLED — still live
        to == OrderState::Filled ||          // confirmed FILLED — terminal
        to == OrderState::Rejected ||        // confirmed REJECTED — terminal
        to == OrderState::Cancelled ||       // confirmed CANCELED — terminal
        to == OrderState::Expired ||         // confirmed EXPIRED or EXPIRED_IN_MATCH
                                              // — terminal (see mapping note below)
        to == OrderState::CancelRequested || // confirmed PENDING_CANCEL — NEW
                                              // (fixes round-6 P1) — still live,
                                              // a cancel is in progress on Binance's
                                              // side; this transition already exists
                                              // elsewhere in the table (Accepted →
                                              // CancelRequested), reused here for
                                              // the case where reconciliation is what
                                              // discovers a cancel is already pending
        to == OrderState::EscalatedToOperator)  // §6.4 — exhausted retries
        return TransitionResult::Ok;
    break;
```

**Status-string → `OrderState` mapping — fixes round-6 P1 (previously only `NEW`/`PARTIALLY_FILLED`/`FILLED` were discussed by name; the remaining Binance-documented statuses were left implicit)**:

| Binance `status` | `confirmed_state` | Terminal? |
|---|---|---|
| `NEW` | `Accepted` | No |
| `PARTIALLY_FILLED` | `PartialFill` | No |
| `FILLED` | `Filled` | Yes |
| `PENDING_CANCEL` | `CancelRequested` | No |
| `CANCELED` | `Cancelled` | Yes |
| `REJECTED` | `Rejected` | Yes |
| `EXPIRED` | `Expired` | Yes |
| `EXPIRED_IN_MATCH` (STP-triggered expiry) | `Expired` | Yes — deliberately folded into the same terminal bucket as `EXPIRED`: both mean "did not end up resting or filled, and is now closed," and `OrderState` has no dedicated self-trade-prevention state. This is a documented simplification, not an oversight — a future revision could split it out if STP-specific handling is ever needed. |
| Any other string (unrecognized/future Binance status) | — | `Inconclusive`, not a guess — an unrecognized `status` value fails §6.1.2's schema validation rather than being mapped to anything; a status this design doesn't know about is not evidence of any particular outcome. |

`apply_confirmed_state()` (§6.2's pseudocode) maps `ReconcileQueryResult::confirmed_state` to the matching `OrderState` and calls `transition_to()` with it — **in-flight/COID release happens only when `is_exchange_final()` is true** (L5 §6.5; this deliberately differs from legacy `is_terminal(EscalatedToOperator)`). A live, resting, cancel-in-progress, or escalated order stays tracked in its appropriate registry/ledger exactly as if it had been reached through the non-ambiguous path.

### 6.6 Open-order status polling — fixes round-8 P0 (deeper: `Found → Accepted` left live orders untracked)

`order_lifecycle.hpp::determine_reconcile_action()` returns `NoAction` for every state other than `Ambiguous`. After §6.5 maps a `Found` result to `Accepted` / `PartialFill` / `CancelRequested`, the order is **still live on the exchange** and still in `InFlightRegistry`, but nothing in round 7's design would ever call `query_order()` on it again. This design has no user-data WebSocket stream; REST status polling is the only fill/cancel discovery path. Without an explicit schedule, resting orders are silently abandoned until process restart (and restart restores them as Accepted/PartialFill — still `NoAction`). That is a real correctness bug, not an operational nicety.

**Fix — a second, distinct poll schedule for non-terminal confirmed states:**

```cpp
// Proposed addition alongside determine_reconcile_action():
enum class OpenOrderPollAction : std::uint8_t {
    NoPoll = 0,          // terminal, or not yet confirmed-live
    PollNow = 1,         // issue one GET /api/v3/order (status refresh)
    EscalateToOperator = 2, // too many consecutive Inconclusive polls
};

inline OpenOrderPollAction determine_open_order_poll(const OrderRecord& rec) noexcept {
    if (rec.state != OrderState::Accepted &&
        rec.state != OrderState::PartialFill &&
        rec.state != OrderState::CancelRequested) {
        return OpenOrderPollAction::NoPoll;
    }
    if (rec.open_poll_failures >= OrderRecord::kMaxOpenPollFailures) {
        return OpenOrderPollAction::EscalateToOperator;
    }
    return OpenOrderPollAction::PollNow;
}
```

Rules:

1. **`query_attempts` is ONLY for Ambiguous reconciliation** (§6.2) and must not be reused for open-order polls — mixing them would either escalate a healthy resting order after 3 status checks, or prevent Ambiguous exhaustion from ever firing. `OrderRecord` gains a separate `open_poll_failures` counter (`kMaxOpenPollFailures = 5`).
2. **Global poll weight budget — fixes round-10 P0 (a flat 1s cadence is arithmetically impossible at capacity):** 64 in-flight × `GetOrder` weight 4 × 60 polls/min = 15,360 weight/min, more than double the entire documented 6,000/min IP REQUEST_WEIGHT budget before a single order, account refresh, or reconciliation is paid for. And the owner thread is a single actor: each `query_order()` may legally block up to its full per-phase deadline budget, so 64 sequential GETs cannot physically complete in one second anyway. Polling therefore runs under an explicit budget, not a cadence promise: `kOpenOrderPollWeightFraction = 0.20` of the *configured* REQUEST_WEIGHT limit per window is the most open-order polling may reserve (tracked by a dedicated `poll_weight_spent_this_bucket` counter beside the main tracker; the main tracker still does the real reservation). RAW_REQUESTS is charged identically (+1 per poll, same fractional cap applied to its limit).
3. **Admission + priority**: pollable orders sit in a fixed-capacity round-robin queue (`kMaxInFlight` slots — one slot per order, membership toggled by state, no allocation). The scheduler dequeues the least-recently-polled eligible order **only when** (a) the actor has no due control-plane task (kill-switch, clock resync, account refresh), (b) no due Ambiguous reconciliation task (§6.2 — reconciliation outranks status polling), and (c) the fractional budget in rule 2 admits the reservation. `kOpenOrderPollIntervalMs = 1000` is a per-order **floor** (never poll the same order more often), not a guarantee.
4. **Staleness alarm + hard-staleness admission gate (round-12 P1) — time basis fixed this round (P1):** each order records `last_poll_completed_utc_ms` (renamed from an earlier revision's unqualified `last_poll_completed_ms` — that name, with no explicit clock basis, invited exactly the bug being fixed here: the DURABLE/checkpoint field this same value round-trips through, §6.1.2's `AuditRecord::last_poll_completed_utc_ms`, is explicitly server-corrected UTC; an in-memory field that a reader could reasonably assume was `steady_clock`-based, restored verbatim from that UTC field after a restart with no conversion, would compare a UTC epoch value against `Clock::now()`'s steady-clock epoch — a basis mismatch producing either nonsense "age" values or, worse, an "age" that happens to look small enough to silently clear the hard-staleness gate after a wall-clock rollback, exactly the fail-open this gate exists to prevent). **Fix**: the in-memory field is `last_poll_completed_utc_ms`, always server-corrected UTC (L4 §2.2's offset, never raw local wall clock), matching the durable field name and basis exactly — restore-after-crash is then a direct copy, no conversion, no basis ambiguity. Age is always computed as `current_server_corrected_utc_ms - last_poll_completed_utc_ms`, never against `steady_clock::now()`. Soft: age > `kOpenOrderPollStalenessMs = 60000` → out-of-band alarm. Hard: if any eligible order's age exceeds `kOpenOrderPollHardStaleMs = 300000` (or the aggregate "polling frozen" condition persists that long), **Gate 6 refuses new order submissions** until the stale set drains below the soft threshold — poll is the only fill/cancel discovery path without user-data WebSocket; silent continued admission while discovery is dead is fail-open on risk.
5. **Reserve → persist-incremented-failures → GET — fixes round-11 P0, ABI fixed this round (P0):** an earlier revision described step 4's `reset_after_seq` as "the polled event's sequence" without any way to actually obtain it — `append_durable()` returned only `Acked`/`Failed`, no sequence number. Fixed via `AuditAppendResult::sequence` (above): the poll event's write in step 2 now yields the exact sequence step 4 needs, captured and threaded through explicitly rather than re-derived:
   1. `try_reserve_weight_only(...)` — failure does not touch `open_poll_failures`; re-queue.
   2. `rec.open_poll_failures++`; `auto poll_result = audit.append_durable(poll_event /* OrderOpenStatusPolled, new count */); if (!poll_result.acked()) { roll back, fence per L5, no GET; return; } const std::uint64_t polled_event_seq = poll_result.sequence;`
   3. `query_order()`.
   4. On `Found`: `apply_open_order_observation(...)` (table below); then append `OrderOpenPollFailuresReset` with `open_poll_failures = 0` and `reset_after_seq = polled_event_seq` — the exact sequence step 2's write was assigned, captured directly from its own `AuditAppendResult`, never guessed or reconstructed from a separately-tracked counter that could drift. ACK required before clearing in-memory count.
   5. On `Inconclusive`: pre-GET increment stands; re-queue with backoff; `return`.
6. **Recovery of `open_poll_failures` — fixes round-12 P1 (max-vs-reset race):** replay that COID's poll-related events in `sequence_number` order. `OrderOpenStatusPolled` sets `failures = event.count`. `OrderOpenPollFailuresReset` sets `failures = 0` only if `event.reset_after_seq` equals the sequence of the latest prior `OrderOpenStatusPolled` (or is ≥ that sequence). A reset that does not satisfy the seq predicate is ignored (Corrupt only if it references a future seq). Taking "global max count" without sequence order is **forbidden** — it would resurrect a pre-reset 5 after a successful reset and false-escalate.
7. **Open-order observation matrix — fixes round-12 P0 (incomplete transitions stuck/false-escalate):**

| Current local state | Exchange `status` | Action |
|---|---|---|
| `Accepted` | `NEW` | Observation only: durable no-op or heartbeat frame; **no** `transition_to()` (`Accepted → Accepted` is not a state-machine edge and must not be fed to `validate_transition`). |
| `Accepted` | `PARTIALLY_FILLED` | `transition_to(PartialFill)` + fill fields (monotonic). |
| `Accepted` | `FILLED` | `transition_to(Filled)` + fill fields. |
| `Accepted` | `CANCELED` | `transition_to(Cancelled)` — **new proposed edge** in `order_lifecycle.hpp` (exchange-initiated cancel without a local `CancelRequested`). |
| `Accepted` | `EXPIRED` / `EXPIRED_IN_MATCH` | `transition_to(Expired)`. |
| `Accepted` | `PENDING_CANCEL` | `transition_to(CancelRequested)`. |
| `PartialFill` | `PARTIALLY_FILLED` (qty ≥ prior) | Fill-progress only (L5 §6.1.4); **no** `transition_to()`. |
| `PartialFill` | `FILLED` | `transition_to(Filled)`. |
| `PartialFill` | `CANCELED` | `transition_to(Cancelled)` — **new proposed edge**. |
| `PartialFill` | `EXPIRED`… | `transition_to(Expired)`. |
| `PartialFill` | `PENDING_CANCEL` | `transition_to(CancelRequested)`. |
| `CancelRequested` | `PENDING_CANCEL` | Observation only; **no** `transition_to()`. |
| `CancelRequested` | `CANCELED` | `transition_to(Cancelled)`. |
| `CancelRequested` | `FILLED` | `transition_to(Filled)` (already legal). |
| `CancelRequested` | `EXPIRED`… | `transition_to(Expired)`. |
| `CancelRequested` | `NEW` | `transition_to(Accepted)` — **cancel failed / withdrawn**; exchange reopened the resting order. Without this edge the order sticks in `CancelRequested` forever and poll escalates falsely. |
| `CancelRequested` | `PARTIALLY_FILLED` | `transition_to(PartialFill)` + fill fields — same cancel-failure / continued-fill case. |
| `Accepted` | `REJECTED` (or other terminal not reachable from Accepted) | `Inconclusive` — schema/lifecycle anomaly; no state change. |
| `PartialFill` | `NEW` | `Inconclusive` — executed qty cannot regress to unfilled; no state change. |
| Any above | schema/field mismatch | `Inconclusive` (counts toward `open_poll_failures`); no state change. |

Proposed `validate_transition` additions (alongside L5's `Submitting → PartialFill` — `AbortedPreSend` was removed entirely from L5 revision 17's normative design: the enum value, its transition, and every behavioral rule that depended on it. **Fixes this round's P1** — the name still appears, correctly, in both files' historical changelog entries documenting rounds that added and then removed it; those are a record of what happened, not a live reference, and are left as-is. No *normative* section of either spec — anything describing current required behavior, not history — references it):

```cpp
case OrderState::Accepted:
    // existing PartialFill/Filled/CancelRequested/Expired, plus:
    if (to == OrderState::Cancelled) return TransitionResult::Ok;
case OrderState::PartialFill:
    // existing Filled/CancelRequested/Expired, plus:
    if (to == OrderState::Cancelled) return TransitionResult::Ok;
case OrderState::CancelRequested:
    // existing Cancelled/Filled/Expired, plus cancel-failure reverse edges:
    if (to == OrderState::Accepted || to == OrderState::PartialFill)
        return TransitionResult::Ok;
```

8. **Terminal**: `Found →` terminal → release in-flight, leave queue. `EscalateToOperator` from open-poll failures → escalated ledger (L5 §6.5).

Direct non-terminal POST outcomes enter this same queue. Progressive fills use the PartialFill row above.

## 7. Rate-limit header accounting (fixes: local-only weight=1 assumption; fixes rev-2 P1: "feed back" had no concrete API)

After **every** response (success or error), parse:

- `X-MBX-USED-WEIGHT-*` (interval-suffixed, e.g. `X-MBX-USED-WEIGHT-1M`) — Binance's authoritative view of request-weight consumption.
- `X-MBX-ORDER-COUNT-*` (e.g. `X-MBX-ORDER-COUNT-10S`, `X-MBX-ORDER-COUNT-1D`) — **a separate rate-limit bucket from request weight**, enforced independently by Binance, specific to order-placing endpoints.
- `Retry-After` (present on `429`/`418` responses, seconds).

Revision 2 said this should "feed back into `RequestWeightTracker`" without defining how. Concretely, `RequestWeightTracker` (`transport_policy.hpp`) gains two new methods, and orchestration wires up a **second, separately-configured instance** for order-count (Binance enforces weight and order-count as independent buckets; one tracker's single-window model cannot represent both):

```cpp
struct BucketIdentity;

class RequestWeightTracker {
public:
    // CHANGED — fixes round-5/8/9 P0 + round-8 P1.
    //
    // Round 5: window duration parameterized.
    // Round 8 P1: window_seconds is 4th param (after TimePoint now) so
    //   existing reset(limit, margin, t0) call sites keep compiling.
    // Round 8 P0: fixed-bucket model, not sliding window.
    // Round 9 P0 (critical ABI gap): bucket rotation is defined on SERVER UTC
    //   boundaries, but every prior method only accepted steady_clock::TimePoint.
    //   A steady_clock reading cannot locate a UTC :00/:10/:20 or midnight
    //   boundary — any implementation would silently fall back to local/steady
    //   time and could release 10S/1M/1D budget early. Fix: every method that
    //   rotates or evaluates a bucket takes an explicit server_now_ms
    //   (local_utc_ms + offset_ms from §2.2). If the clock offset is missing
    //   or stale (§2.2 fail-closed), callers MUST NOT call try_consume — they
    //   refuse the send at the gate. There is no "fall back to steady_clock
    //   for bucket math" path.
    void reset(std::uint32_t limit, std::uint32_t safety_margin,
               TimePoint now = Clock::now(),
               std::uint32_t window_seconds = 60) noexcept;

    // server_now_ms: required. steady_now: freeze/deadline comparison only.
    bool try_consume(std::uint32_t weight,
                     std::int64_t server_now_ms,
                     TimePoint steady_now = Clock::now()) noexcept;
    bool can_send(std::uint32_t weight,
                  std::int64_t server_now_ms,
                  TimePoint steady_now = Clock::now()) const noexcept;
    void rollback(std::uint32_t weight,
                  std::int64_t server_now_ms,
                  TimePoint steady_now = Clock::now()) noexcept;

        // REQUEST_WEIGHT: used := max(local, server_reported) — BUT only when
    // server_bucket_start_ms == local bucket_start (round-11 P0). See §7.1.
    // ORDERS (via OrderCountTrackerSet): used := server_reported exactly,
    // same bucket-identity gate.
    void correct_from_response_header(std::uint32_t server_reported_used_weight,
                                       BucketIdentity identity,
                                       std::int64_t server_now_ms,
                                       TimePoint steady_now = Clock::now()) noexcept;

    // In-process freeze (steady_clock). Restart-surviving deadline is the
    // durable RateLimitFreeze frame in §10 / §7.3.1 (UTC wall-clock ms).
    void freeze_until(TimePoint until) noexcept;
    bool is_frozen(TimePoint steady_now = Clock::now()) const noexcept;

private:
    std::uint32_t window_seconds_{60};
    std::int64_t bucket_start_server_ms_{0};
    CorrectionMode correction_mode_{CorrectionMode::MaxOfLocalAndServer};
};

enum class CorrectionMode : std::uint8_t {
    MaxOfLocalAndServer = 0,  // REQUEST_WEIGHT / RAW_REQUESTS
    ServerAuthoritative = 1,  // ORDERS (fills may lower the count)
};
```

**Existing `transport_policy.hpp` call sites** that today call `try_consume(weight)` / `can_send(weight)` without `server_now_ms` are updated by this proposed change — they must obtain server time from the same §2.2 offset used for signing (`server_now_ms = local_utc_ms + offset_ms`) or refuse. A defaulted `server_now_ms` that silently uses local wall time is **forbidden** (would reintroduce the round-9 fail-open).

### 7.0 Bootstrap phase — fixes round-10 P0 (cold-start reservation deadlock)

Round 9's "reserve before **every** REST call" rule, taken together with "reservation requires `server_now_ms` and `exchangeInfo`-configured trackers," is circular at first startup: `server_now_ms` needs `GET /api/v3/time`, tracker limits need `GET /api/v3/exchangeInfo` — and both of those GETs would themselves need a reservation that cannot yet be computed. Without an explicit bootstrap definition, an implementation either deadlocks fail-closed forever (never starts) or quietly invents an exception (fail-open). Both are wrong; the bootstrap phase is defined, bounded, and strictly conservative:

**Phase B (bootstrap) — active from process start until all three of: clock offset published (§2.2), `exchangeInfo` limits configured, ORDERS baseline fetched (§7.1.1).**

0. **Crash / unknown-usage gate BEFORE any Phase-B network I/O — fixes round-11/12 P0.** Round 10's "halved bootstrap limits + allow `/time` immediately" is not safe. Round 11's "wait on local UTC boundary + 120s pad" is also not safe: there is no proof of the host's maximum wall-clock skew; a local clock fast beyond the pad can end the wait while the server is still inside the predecessor's exhausted bucket, so the first `/time` still trips 429/418. Rule (see §7.5.1):
   - **Crash path / no trustworthy usage snapshot**: wait a fixed **steady_clock** duration from process start — `kCrashBootstrapWait = max_pinned_ip_window_seconds + kCrashBootstrapPadSeconds` (defaults: 300 + 120 = 420s of real elapsed time). **No wall-clock / UTC boundary math** participates in this wait; wall skew cannot shorten it. **Zero network I/O during this wait**, including `/time`. After the wait, Phase B proceeds.
   - **Clean-shutdown snapshot still plausibly in-bucket**: before the first `/time`, seed every bootstrap tracker with `snapshot.used + kRestartPad` and reserve `/time` against that upper bound. This may over-refuse after rollover but never sends a first packet as if prior IP usage were zero. If this conservative reservation cannot fit the pinned bootstrap half-limit, do not burn retries: perform one complete `steady_clock` longest-window + pad wait, discard the snapshot, and re-enter the crash-safe Phase-B path. `/time` may relax/re-bucket the snapshot only after server time confirms the bucket.
   - **First-ever start** (no breadcrumb, no store — §10.2): no wait; Phase B proceeds under halved bootstrap limits (no predecessor).
   - Any recovered durable freeze (§7.3.1) dominates and is applied before the wait begins.
1. **Pinned conservative bootstrap limits.** All three trackers start in `Bootstrap` mode with hardcoded limits taken from Binance's *published defaults* for `api.binance.com`, reduced by a fixed pad: `kBootstrapWeightLimit = 6000/min × ½`, `kBootstrapRawLimit = 61000/5min × ½`, ORDERS = **zero** (no order may ever be placed during bootstrap — ORDERS budget is simply not available until §7.1.1's baseline succeeds). These constants live next to `kPinnedEndpointWeight` (§7.4). The half is a second line of defense for *first-ever* / *clean-snapshot* starts only — it is **not** the crash-path defense (rule 0 is).
2. **No bucket rotation while server time is unknown.** In `Bootstrap` mode a tracker performs **no rotation at all** — consumption accumulates monotonically against the bootstrap limit, as if the window never resets. Callers in Phase B pass the sentinel `server_now_ms = -1`; a `Bootstrap`-mode tracker ignores it, and a *configured* tracker receiving `-1` refuses the send. There is no local-clock fallback path for rotation.
3. **Bootstrap endpoint whitelist.** The only endpoints callable in Phase B, in order: `GetServerTime` → `GetExchangeInfo` → `GetRateLimitOrder` (each still reserving from the bootstrap budgets via `try_reserve_weight_only()`, at pinned weights). `PostOrder`, `GetOrder`, `GetAccount` are structurally refused in Phase B. Total worst-case bootstrap cost is pinned and tiny: 1 + 20 + 40 = 61 weight, 3 raw requests — and on the crash path it is spent only *after* the IP windows have rolled over.
4. **Bounded retries.** Each bootstrap step retries with the §6.3 backoff schedule up to `kMaxBootstrapAttemptsPerStep = 5`; exhaustion → operator escalation and no transition out of Phase B.
5. **Carry-over on exit.** When Phase B completes, each tracker is switched to configured mode by: applying the real `exchangeInfo` limits, seeding `used` with everything consumed during bootstrap (never zeroed), aligning the first real bucket per §7.1.2's late-rotation rule, and applying any pending header corrections (§7.1 bucket-identity rule).

This resolves the circularity without weakening anything: every bootstrap request is still reserved, still weight-accounted, still frozen by 429/418, and the order path stays closed until real limits and baselines exist. One Phase-B-specific conservatism: a durable freeze deadline written *before* the clock offset exists is computed from **local** UTC + `Retry-After` and therefore carries unknown offset error — Phase B adds `kBootstrapFreezePadMs = 60000` to any freeze deadline it records, so a fast local clock can never shorten a real ban.

### 7.1 Order-count needs one tracker per interval, not "a second tracker" (fixes round-3 P1) — and each interval's window duration must match its actual Binance meaning, not be silently forced to 60s (fixes round-5 P0)

Round 2 proposed "a second, distinctly-configured instance" for order-count — but Binance reports order-count across **multiple simultaneous intervals** (e.g. `X-MBX-ORDER-COUNT-10S` *and* `X-MBX-ORDER-COUNT-1D` in the same response), each an independent limit. A single second tracker can only represent one window; it can't represent both at once.

Round 4 fixed `correct_from_header()`'s return type but, verified against the actual `RequestWeightTracker` source, missed that `IntervalTracker::tracker` reusing `RequestWeightTracker` "as-is" inherits its **hardcoded 60-second window** — silently wrong for every interval except one that happens to be exactly 60 seconds. A `1D` bucket's usage would decay to zero after one real minute (Binance's own daily cap would then be bypassed locally — a true fail-open, not just an inaccurate estimate), and a `10S` bucket's timing would be equally wrong in the other direction. This revision fixes it by (a) parameterizing `RequestWeightTracker`'s window (above) and (b) parsing each interval's actual duration from its suffix and configuring the tracker with it:

```cpp
// NEW — parses "10S" → 10, "1D" → 86400, etc. Binance's published unit
// letters are S(econd)/M(inute)/H(our)/D(ay). Returns false (fail closed —
// treat as CapacityExhausted, never silently guess a window) for any suffix
// that doesn't parse as {digits}{one of SMHD}.
inline bool parse_interval_seconds(std::string_view interval_suffix,
                                    std::uint32_t& out_seconds) noexcept;
```

`configure_limit()` and the never-before-seen-interval path inside `correct_from_header()` both call `parse_interval_seconds()` and pass the result to that interval's `IntervalTracker::tracker.reset(limit, /*safety_margin=*/0, now, window_seconds)` — note argument order (`now` before `window_seconds`, §7 above). Order-count tracking uses `safety_margin = 0` deliberately (Binance's header already reports the server's own authoritative used-count). A suffix that fails to parse is treated identically to `CapacityExhausted` (§7.1's existing latch) — never defaulted to 60 seconds.

**All intervals use server-aligned fixed buckets — fixes round-8 P0 (round 6 only aligned `>= 3600s`; short intervals stayed on a relative sliding window that locally decays early):** Binance documents fixed, server-aligned intervals for every `rateLimitType` (`REQUEST_WEIGHT` and `ORDERS` alike): a `1 MINUTE` bucket starts every minute; a `10 SECOND` bucket resets at 0/10/20… seconds; a `1 DAY` bucket resets at 00:00 UTC. A relative sliding window that fractionally decays prior-bucket usage *inside* the current bucket will locally report headroom Binance has not yet granted — a real fail-open, not a cosmetic drift. Therefore:

```cpp
// bucket_start = floor(server_now_ms / (window_seconds * 1000)) * (window_seconds * 1000)
// Rotation is LATE, never early (§7.1.2): the new bucket is adopted only once
// server_now_ms_pessimistic(...) has crossed the boundary; only then
// used_current_ = 0 and bucket_start updates.
// Within a bucket: used_current_ is a pure counter — NO fractional decay of a
// "previous" bucket. correct_from_response_header() may raise or (for ORDERS)
// lower the counter toward the server-reported value — but ONLY for a matching
// bucket identity (round-11 P0 below).
inline std::int64_t aligned_bucket_start_ms(std::int64_t server_now_ms,
                                             std::uint32_t window_seconds) noexcept;
```

`server_now_ms` for signing is the point estimate from `ClockOffsetSnapshot` (§7.1.2); rotation decisions use `server_now_ms_pessimistic` exclusively. Never bare `steady_clock`.

**Header correction must carry server-bucket identity — fixes round-11/12 P0:** late rotation means the local tracker can still be on bucket *N* while the server is already on *N+1*. Round 11 computed `server_bucket_start_ms` from the local **point estimate** (or an optional `Date`) and treated that as identity fact — but when the estimate sits within `error_bound` of a boundary, the server may already be in N+1 while the client still classifies N; applying N+1's used-count into N, then rotating and zeroing, re-opens the exact fail-open window. Fix — **identity humility**:

```cpp
enum class BucketIdentityKind : std::uint8_t {
    Trusted = 0,   // verified server time, not near a boundary
    Unknown = 1,   // must NOT be used as an apply key
};

struct BucketIdentity {
    BucketIdentityKind kind{BucketIdentityKind::Unknown};
    std::int64_t bucket_start_ms{0};  // meaningful only if Trusted
    std::uint64_t proof_generation{0}; // meaningful only if Trusted
};
```

1. **Classify identity at response receipt** for each header interval:
   - **An ordinary HTTP `Date` is never sufficient.** The spec has no proof that an edge/proxy Date and Binance's IP-limit accounting node share a clock. `Trusted` is possible only from an authenticated dedicated `/api/v3/time` response that produced the currently-held `ClockOffsetSnapshot`; every ordinary endpoint response is `Unknown`, regardless of Date.
   - For that bound `/time` proof, `candidate = aligned_bucket_start_ms(T, window)`. If `T` is within `error_bound_ms + 1000ms` of a boundary, remain `Unknown`; otherwise emit `Trusted{candidate, proof_generation}`. The implementation records the proof generation alongside the correction so a later offset publication cannot silently reinterpret it.
   - The local point estimate alone **never** produces `Trusted`. Estimates may drive late *rotation* (pessimistic); they must not drive correction *identity*.
2. `correct_from_response_header(..., BucketIdentity id, used, ...)`:
   - `Trusted` and `id.bucket_start_ms == local bucket_start`: apply (`max` / `:=` as today).
   - `Trusted` and `id.bucket_start_ms > local`: pending-correction slot (as round 11); seed on adopt.
   - `Trusted` and `id.bucket_start_ms < local`: ignore + operator alarm; never lower local used.
    - **`Unknown` (round-13 P0)**: do **not** apply into any local bucket and do **not** invent a pending bucket key from the estimate. Set `identity_hold_ = true` on that tracker: ordinary `try_consume` / `try_reserve_*` return false until a subsequent response yields `Trusted` for this interval. The only escape is the dedicated, pre-reserved `TimeResyncCredit` below — **except** while an uncleared freeze epoch is active, in which case round-28 forbids `TimeResyncCredit` network sends and the escape is §7.3.1 `FreezeProbeCredit` only; there is no estimate- or `Date`-based escape hatch.
3. `OrderCountTrackerSet::correct_from_header(interval_suffix, used, BucketIdentity, server_now_ms, ...)` uses the same rule per interval; the old `{server_bucket_start_ms, server_now_ms}` signature is retired and must not be retained as an overload.
4. Fault-injection must include: clock estimate within `error_bound` of a 10S/1M boundary + weight header present + no trusted `Date` → `Unknown` hold, no wipe-on-rotate reopen.

**Time-resync credit (round-13 P0; prevents an Unknown hold self-deadlock) — bound to the live weight config, fixed this round (P1):** an earlier revision sized this credit from a fixed `kTimeResyncWeight` constant, independent of §7.4's `EndpointWeightConfig` — but §7.4 explicitly allows an operator to load a versioned config that changes `GetServerTime`'s weight. A hardcoded credit that doesn't track that config could under-provision the one reservation this design guarantees will always be enough (reserving too little for the actual current cost of `/time` would make the resync itself fail exactly when the Unknown-hold self-deadlock fix needs it to succeed) — the one lane in this entire design that cannot be allowed to silently drift out of sync with the cost it's meant to cover. **Fix**: `TimeResyncCredit`'s reserved amount is `endpoint_request_weight(PrivateRestEndpoint::GetServerTime, current_cfg)` (§7.4), not a compile-time constant — re-evaluated and re-reserved (topping up or trimming the lane) every time `EndpointWeightConfig` changes (an operator-loaded update, §7.4), not just once at bootstrap. Configured mode permanently reserves this amount plus one RAW-request credit in a separate fixed-capacity `TimeResyncCredit` lane. Only `GET /api/v3/time` may consume it; it is not an ordinary tracker bypass and cannot be borrowed by order/account/reconciliation traffic. A successful `/time` that supplies `Trusted` identities for every configured REQUEST_WEIGHT/RAW interval replenishes it. If that response is boundary-ambiguous, the same lane schedules exactly one retry after `max(error_bound_ms + 1000ms, kTimeResyncMinRetryMs)` of steady time; this retry does not require ordinary tracker reservation. Exhaustion, a second boundary-ambiguous retry, or no trustworthy result before `kTimeResyncDeadlineMs` (default 60s) leaves L4/L5 held and alarms. The actor coalesces all waiters into one outstanding `/time`; no retry storm or heap queue is permitted.

**Round-28 P0 — freeze / TimeResyncCredit mutual exclusion (normative):** `TimeResyncCredit` exists **only** for the clock-identity-`Unknown` case **when no uncleared freeze epoch is active**. While `recover_control_plane` / live state reports an uncleared freeze (`out_has_freeze` or `out_permanent_latch`, or in-process active episode):
- `TimeResyncCredit` **MUST NOT** issue any network `/time` (including "once per boot," §7.5.1 first-packet, or Unknown-hold escape).
- All freeze-period `/time` — clock re-publish after crash, deadline probes, identity refresh — go through **`FreezeProbeCredit` + durable `FreezeProbeAttempt`** exclusively (§7.3.1): Ack-before-send, epoch-scoped 8-cap, restored across restart. There is **one** recoverable network-probe budget per frozen episode (the 8-cap); no parallel free lane.
- Round 27's "once per boot TimeResyncCredit while frozen" is **withdrawn** — it was a restart-launderable bypass.

**ORDERS fill decrements — conservative local rule (fixes round-8 P0 companion):** Binance's unfilled-ORDERS count decrements when an order fills (partially or fully), with a documented short delay. A local tracker that only ever increments on `try_reserve_all()` would over-count between corrections (fail-closed — safe but tight); one that guesses a decrement from a local `Filled`/`PartialFill` observation can under-count if the guess is wrong or early (fail-open — unsafe). **Rule**: this process NEVER locally decrements ORDERS count based on fill observations or inferred fills. Only `OrderCountTrackerSet::correct_from_header()` / the startup `GET /api/v3/rateLimit/order` baseline may change the count, and those paths **assign `used := server_reported` exactly** (including decreases) — subject to the bucket-identity gate above. Between corrections the reserved local count is treated as an upper bound that only a server header may lower. Near the limit, prefer an extra `rateLimit/order` refresh over guessing — Binance documents a short delay between fill and count update.

```cpp
// NEW — fixes round-4 P1-1: correct_from_header() previously returned void,
// giving callers and tests no way to observe or assert on the capacity-
// exhausted / fail-closed path the struct's own comment already promised.
enum class OrderCountCorrectionResult : std::uint8_t {
    Applied = 0,           // known interval, corrected normally
    NewIntervalRegistered = 1,  // never-seen-before interval, a free slot existed
    CapacityExhausted = 2, // never-seen-before interval, no free slot — this
                            // set is now permanently in a fail-closed state
                            // (see can_send_all() below) until process restart,
                            // since a slot that can't be tracked can't later be
                            // proven safe either
};

// A small, fixed-capacity set — no heap allocation, matching this codebase's
// engineering discipline. Bounded to Binance's actual published interval
// count (currently well under 8); intervals are discovered from whichever
// X-MBX-ORDER-COUNT-* headers are actually present in a response, not
// hardcoded to a specific set that could go stale if Binance adds one.
class OrderCountTrackerSet {
public:
    // Configures the known limit for one interval, from
    // GET /api/v3/exchangeInfo's rateLimits[] entries of type ORDERS —
    // fixes round-7 P0: earlier revisions named this field "ORDER_COUNT",
    // which does not match Binance's actual documented rateLimitType enum
    // (REQUEST_WEIGHT / ORDERS / RAW_REQUESTS). Filtering rateLimits[] for a
    // type string that doesn't exist in the real response would silently
    // match ZERO entries — configure_limit() never fires for any interval,
    // and this set's own can_send_all()/try_reserve_all() (below) would then
    // vacuously permit every send, since "no configured intervals to check"
    // is not the same as "every interval permits sending" — a real fail-open
    // this revision also closes explicitly (see below).
    // headers report USED count, never the limit itself, so the limit must
    // come from here, set once at registry refresh (L4 §5), before any
    // header correction is applied. Calling this for an interval already
    // configured overwrites its limit (Binance can change limits between
    // exchangeInfo refreshes); it never creates a second slot for the same
    // interval_suffix. Internally calls parse_interval_seconds(interval_suffix)
    // and configures that slot's tracker with the resulting window — fixes
    // round-5 P0 (was silently inheriting RequestWeightTracker's hardcoded
    // 60s window regardless of what this interval actually means). If the
    // suffix fails to parse, this call is a no-op (never creates/updates a
    // slot with a guessed window) — the interval stays absent until a valid
    // suffix is seen, at which point correct_from_header()'s own fail-closed
    // path below applies.
    void configure_limit(std::string_view interval_suffix,
                          std::uint32_t limit) noexcept;

    // Parses the interval suffix (e.g. "10S", "1D") from the header name and
    // corrects that interval's tracker — creating a slot for a
    // never-before-seen interval if capacity allows (this can happen if a
    // response reports an interval configure_limit() hasn't seen yet). The
    // new slot's window is set from parse_interval_seconds(interval_suffix);
    // if that fails to parse, OR if capacity is exhausted, this returns
    // CapacityExhausted (§7.1's unknown-window and no-free-slot cases are
    // both "cannot safely track this interval," handled identically). A
    // successfully-created new slot still starts with limit == 0, i.e.
    // already at capacity, until the next exchangeInfo refresh calls
    // configure_limit() for it — fail closed on the unknown limit rather
    // than assuming headroom. Returns which case applied so callers/tests
    // can assert on it directly.
    OrderCountCorrectionResult correct_from_header(
        std::string_view interval_suffix,
        std::uint32_t used_count,
        BucketIdentity identity,
        std::int64_t server_now_ms,
        TimePoint now = Clock::now()) noexcept;

    // ALL known intervals must independently permit sending — one exhausted
    // bucket blocks the send even if every other interval has headroom.
    // Also returns false, permanently, once any correct_from_header() call
    // has returned CapacityExhausted — an interval this set couldn't track
    // can never be proven safe again within this process's lifetime.
    //
    // FIXES ROUND-7 P0: an empty interval set (zero active slots — e.g. if
    // configure_limit() never fired because it filtered for the wrong
    // rateLimitType, per the fix above, or simply because startup hasn't
    // completed the exchangeInfo/baseline fetch yet) must NOT vacuously
    // return true. "All active intervals permit sending" is trivially true
    // over an empty set — a loop that only checks active slots and finds
    // none to object to is a real fail-open, not a benign edge case. This
    // method therefore returns false whenever intervals_ has zero active
    // slots, in addition to the existing per-interval and
    // capacity-exhausted checks — "I don't know of any limits yet" is
    // treated identically to "I know of a limit and it's exhausted," never
    // as "no limits exist."
    bool can_send_all(std::int64_t server_now_ms,
                       TimePoint steady_now = Clock::now()) const noexcept;

    // NEW — fixes round-6 P0 (fail-open gap): can_send_all() alone is a pure
    // check with no side effect. Binance consumes order-count budget the
    // moment a POST /api/v3/order request is RECEIVED by their system — not
    // only on success — so this process's local view must be updated at the
    // moment it DECIDES to send, not only after a response header confirms
    // it minutes-equivalent later. try_reserve_all() atomically checks
    // can_send_all() AND, if true, increments every known interval's local
    // used-count by 1 BEFORE returning — single L5 owner thread (L4 §9), so
    // "atomic" here means "one indivisible step on that thread," not a
    // hardware atomic. The reservation is NEVER rolled back on a later
    // network failure/timeout/Ambiguous outcome — Binance already counted
    // the attempt against the account the moment the request was sent,
    // regardless of what this process later learns about the outcome; only
    // correct_from_header() (an actual response) is allowed to correct the
    // local count, never a local "undo." Returns false (nothing reserved,
    // nothing sent) if any interval was already at capacity.
    bool try_reserve_all(std::int64_t server_now_ms,
                          TimePoint steady_now = Clock::now()) noexcept;

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
        std::uint32_t configured_limit{0}; // from configure_limit(); 0 == unknown,
                                             // treated as already-exhausted
        RequestWeightTracker tracker{};  // reused as-is per interval
        bool baseline_confirmed{false};  // NEW — see "startup baseline" below;
                                          // false until at least one REAL
                                          // response header has been observed
                                          // for this interval in this process
    };
    std::array<IntervalTracker, kMaxIntervals> intervals_{};
    bool capacity_exhausted_{false};  // latched true by CapacityExhausted;
                                       // makes can_send_all() fail closed for
                                       // the rest of this process's lifetime
};
```

#### 7.1.1 Startup baseline — fixes round-6 P0, corrected in round 7 (round 6's "no such endpoint exists" claim was wrong)

`OrderCountTrackerSet::reset()`/`configure_limit()` start every interval's local used-count at 0 — correct for a genuinely fresh account, wrong for a process restart mid-day: Binance's order-count limits are **account-wide and server-side**, tracked independently of which process or connection made the requests. A restarted L5 process, or a second client using the same API key, has already consumed some of the account's real budget that this process's fresh-reset local tracker knows nothing about — `can_send_all()` would report headroom that doesn't actually exist.

Round 6 claimed no Binance endpoint reports current order-count usage without consuming any, and worked around that with an "unconfirmed long-interval baseline, first send goes out blind" rule — that claim was wrong, and round 7's review correctly caught it. **Binance publishes `GET /api/v3/rateLimit/order` (`rateLimitType=ORDERS`, `SIGNED`, weight-only — it does not itself consume order-count budget) specifically to report current usage across every interval Binance tracks for the account, with no order placed.** The fix is to actually use it, not to route around a gap that doesn't exist:

```
GET /api/v3/rateLimit/order?recvWindow={ms}&timestamp={now}&signature={...}
```

Same signing pipeline as everything else in this spec (§2). Response is an array of per-interval `{rateLimitType, interval, intervalNum, limit, count}` objects — `count` is exactly the authoritative used-count `OrderCountTrackerSet` needs to seed every interval's baseline.

**Fix — L5 startup sequence, before any `POST /api/v3/order` is permitted:**

1. Fetch `GET /api/v3/rateLimit/order`. Parse each returned interval, and for each, call `OrderCountTrackerSet::correct_from_header()`-equivalent seeding (the same code path a response header correction would use, given the same `{interval_suffix, used_count}` shape) — this is the process's real starting baseline, not an assumption.
2. If this fetch fails for any reason (network error, schema mismatch, non-200, timeout — the same exhaustive failure taxonomy §6.1 already applies to `GET /api/v3/order`, reused here rather than inventing a new one) — **L5 does not start.** No `POST /api/v3/order` is permitted until a successful baseline fetch succeeds. This replaces round 6's "first send goes out blind" acceptance: there is no longer a reason to accept that risk, since the authoritative baseline is one signed GET away.
3. Only after step 1 succeeds does `can_send_all()`/`try_reserve_all()` (§7.2) begin permitting sends — consistent with, not an exception to, this set's existing "unconfigured interval = not safe to send" rule (§7.1's fix above).

This baseline fetch is itself subject to `RequestWeightTracker`'s existing weight budget (it has a real, documented weight cost) but **not** to `OrderCountTrackerSet`'s budget, since it is explicitly not an order-count-consuming endpoint — consistent with why it's the right tool for this job in the first place.

#### 7.1.2 Server-time error bound + consistent snapshot — fixes round-10/11 P0

`server_now_ms = local_utc_ms + offset_ms` (§2.2) is an estimate: the offset is derived from one `GET /api/v3/time` round trip and assumes the response was observed at RTT/2. An unbounded estimate near a `10S`/`1M`/`1D` boundary can rotate early. Round 10 published a bound but as a **second independent atomic** written after `offset_ms` — a reader can still observe `{new_offset, old_smaller_bound}` (the tear that "offset first, bound second" was supposed to make "only larger bound" fails when the *new* offset is larger/ahead and the *old* bound is smaller: that pair is *more* aggressive, not more conservative). That is a real early-rotation fail-open under concurrent resync.

**Fix — immutable snapshot publish (round-12 P0: seqlock-over-plain-fields is UB):**

A seqlock that writes/reads non-atomic `ClockOffsetSnapshot` fields concurrently with an atomic `seq` is a C++ **data race** (undefined behavior), even if the algorithm "retries." Two independent `std::atomic<int64_t>` fields are also insufficient (§ revision 11). The only acceptable publish protocols here:

```cpp
struct ClockOffsetSnapshot {
    std::int64_t offset_ms{0};
    std::int64_t error_bound_ms{0};   // = rtt_ms/2 + kClockSlopMs (100)
    std::int64_t system_at_fetch_ms{0}; // §2.2 wall-jump baseline
    std::int64_t steady_at_fetch_ms{0}; // sampled with system_at_fetch_ms
    std::uint32_t seq{0};             // Monotonic publish generation; 0 =
                                        // never published. Bumped by exactly 1
                                        // on each successful publish().
                                        // Round-25 P1: at UINT32_MAX, publish()
                                        // MUST refuse (no wrap) — fence and
                                        // require operator migration; wrapping
                                        // would collide with sentinel 0 and
                                        // break FreezeTimeProbeProof/audit seq.
};

// CANONICAL publish path — fixes this round's P1 (reordered): a
// std::mutex guarding a single ClockOffsetSnapshot instance BY VALUE (copy
// out under the lock, no pointer/reference escapes it) is the default here,
// not std::atomic<std::shared_ptr<...>>. Reasoning: EVERY load() on an
// atomic shared_ptr — even a pure read — performs an atomic reference-count
// increment/decrement pair under the hood (libstdc++/MSVC's implementations
// both do this; it is not lock-free on every platform this codebase targets
// despite the syntax suggesting otherwise), which is real, measurable
// contention/traffic this codebase's own "no unnecessary atomic churn"
// discipline (CLAUDE.md) would flag anywhere it actually matters. This
// structure genuinely is off the hot path (owner actor + occasional resync
// thread, §9) — but "not literally the hot path" is not the same bar as
// "cheap enough that the extra machinery is justified," and a mutex is
// simpler, matches `SymbolRegistry`'s own already-established
// std::shared_mutex choice for a structurally similar problem (§5.3), and
// requires no atomic-shared_ptr reasoning to verify correct. Use the mutex
// form unless a specific, benchmarked call site proves contention that a
// mutex measurably can't handle — a decision to make with profiling data at
// implementation time, not preemptively here.
//
// ALTERNATIVE (only if profiling justifies it): C++20
// std::atomic<std::shared_ptr<const ClockOffsetSnapshot>> — immutable
// snapshots, writer allocates a new const snapshot then store(release),
// readers load(acquire) and hold the shared_ptr for the duration of use.
// Acceptable ONLY for a confirmed single-owner-actor-reads-occasionally
// pattern exactly like this one — never adopted elsewhere in this codebase
// as a general "avoid a mutex" pattern without the same profiling-first
// justification. Plain seqlock over non-atomic fields remains FORBIDDEN
// under either choice (C++ data race / UB regardless of an atomic `seq`).
class ClockOffsetPublisher {
public:
    // false: publication failed. Retain old snapshot and fail closed when it
    // is no longer trustworthy. Caller-supplied `snap.seq` is IGNORED;
    // publish() assigns `seq = published_.seq + 1` under the lock.
    // Round-25 P1: if `published_.seq == UINT32_MAX`, return false immediately
    // (do not wrap); raise fence/alarm — operator migration required before
    // further clock publishes. Never allocate on this path.
    bool publish(ClockOffsetSnapshot snap) noexcept;
    // Returns a BY-VALUE copy taken under the lock — never a pointer/
    // reference into published_, so the caller's copy is immune to a
    // concurrent publish() no matter how long the caller holds it (same
    // by-value-snapshot pattern SymbolRegistry::current_rules() already
    // uses, §5.3). The returned copy's `.seq` is exactly what
    // §7.3.1's FreezeTimeProbeProof::clock_snapshot_seq/clock_offset_ms
    // are captured from, inline, at proof-construction time (below) — this
    // class never needs to answer "what was seq N's offset" for some PAST
    // N; only "what is the current seq/offset right now," which `load()`
    // already provides.
    ClockOffsetSnapshot load() const noexcept;
private:
    mutable std::mutex mu_;
    ClockOffsetSnapshot published_{};
    // ALTERNATIVE, only with profiling justification (see comment above):
    //   std::atomic<std::shared_ptr<const ClockOffsetSnapshot>> published_{};
    // — changes load()'s return type to std::shared_ptr<const ClockOffsetSnapshot>
    // and publish() to allocate+store(release); not the default.
};

// Round-25 P1 — checked int64 arithmetic for clock math (same discipline as
// account_truth checked_*; bare signed +/− is UB on overflow).
inline bool checked_add_i64(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept;
inline bool checked_sub_i64(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept;

// Resync whose rtt_ms > kMaxUsableRttMs (= 2000) is DISCARDED.
// Returns false on overflow → caller MUST treat bucket identity as Unknown,
// hold corrections, and refuse signing (fail-closed — never invent a time).
inline bool server_now_ms_pessimistic(const ClockOffsetSnapshot& s,
                                       std::int64_t local_utc_ms,
                                       std::int64_t& out) noexcept {
    std::int64_t mid = 0;
    if (!checked_add_i64(local_utc_ms, s.offset_ms, mid)) return false;
    return checked_sub_i64(mid, s.error_bound_ms, out);
}
inline bool server_now_ms_for_signing(const ClockOffsetSnapshot& s,
                                       std::int64_t local_utc_ms,
                                       std::int64_t& out) noexcept {
    return checked_add_i64(local_utc_ms, s.offset_ms, out);
}
```

Rotation rule: a tracker rotates from bucket *N* to *N+1* only when `server_now_ms_pessimistic(...)` succeeds **and** `out >= boundary(N+1)`. If the checked call fails → **no rotation**, `BucketIdentity::Unknown` / hold (never early). Consequences, deliberately one-sided:

- **Never early**: local budget refreshes only after even the most pessimistic reading has crossed the boundary.
- **Consumption near a boundary**: charged to the still-local-old bucket (over-count → fail-closed).
- **Header correction identity** is independent and stricter (§7.1): estimates never produce `Trusted` bucket identity.
- **Signing**: `server_now_ms_for_signing` failure → refuse the signed request (no forged timestamp).
- §7.5.1's crash wait no longer uses wall/offset math at all (steady_clock duration).

### 7.2 Orchestration wiring — a single, unified reservation call, fixes round-7 P0

Round 6 gave `OrderCountTrackerSet` a reserving `try_reserve_all()` but left `RequestWeightTracker`'s side of Gate 8 as the older, non-reserving `can_send()` — meaning weight was still only corrected reactively from response headers, the exact fail-open class round 6 fixed for order-count but left half-done for weight. `RequestWeightTracker::try_consume()` (existing code) is already a *reserving* call (it increments `used_current_` on success — it was simply never the one Gate 8 used); pairing it with `OrderCountTrackerSet::try_reserve_all()` still isn't enough on its own, because the two reservations are only correct together if a failure of one is rolled back against the other — reserving weight, then discovering order-count is exhausted, must not silently leave the weight reservation in place for a request that's about to NOT be sent.

```cpp
// NEW — the single call site Gate 7 (§3 of the L5 spec) uses; no other code
// path independently calls try_consume()/try_reserve_all() for an
// order-placing request. (Round 10 removed the -1021 re-POST path entirely —
// L5 §4.5 — so Gate 7→10 is the ONLY order-placing path, full stop.)
// Round-12: weight and raw are multi-interval *sets*.
inline bool try_reserve_all_budgets(RequestWeightTrackerSet& weight,
                                     OrderCountTrackerSet& order_count,
                                     RawRequestsTrackerSet& raw,
                                     std::uint32_t request_weight,
                                     std::int64_t server_now_ms,
                                     TimePoint steady_now = Clock::now()) noexcept {
    if (!weight.try_reserve(request_weight, server_now_ms, steady_now)) {
        return false;
    }
    if (!raw.try_reserve(/*count=*/1, server_now_ms, steady_now)) {
        weight.rollback(request_weight, server_now_ms, steady_now);
        return false;
    }
    if (!order_count.try_reserve_all(server_now_ms, steady_now)) {
        weight.rollback(request_weight, server_now_ms, steady_now);
        raw.rollback(/*count=*/1, server_now_ms, steady_now);
        return false;
    }
    return true;
}
```

**All three** sets' reservations (REQUEST_WEIGHT, RAW_REQUESTS, ORDERS) must succeed via this single function at L5 Gate 7, before format/sign and immediately before the durable Prepared boundary — never a bare `can_send()` (non-reserving). Round 10 removed the `-1021` clock-resync re-POST (`SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` §4.5 — a post-send `-1021` now routes to `Ambiguous` + reconciliation like every other post-send uncertainty), so Gate 7→10 is the *only* order-placing path in the entire design; rounds 7–9's concern about a second send path bypassing reservation is now closed structurally, not by discipline. A `429`/`418` calls `freeze_all_until()` on **`RequestWeightTrackerSet`, `RawRequestsTrackerSet`, and `OrderCountTrackerSet`** (every slot) **and** durably ACKs a `RateLimitFreeze` frame (§7.3.1) — the response doesn't indicate which bucket triggered it, so the conservative response is to freeze everything until `Retry-After` elapses, and that freeze must survive restart.

### 7.3 `Retry-After` parsing contract (fixes round-5 P1)

No previous revision defined what happens when a `429`/`418` response's `Retry-After` header is missing, malformed, negative, zero, or absurdly large — every call site (§6.3's reconciliation backoff, `freeze_until()`/`freeze_all_until()`'s `until` argument, `SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` §4.4's `RateLimited` routing) has so far assumed a well-formed positive integer arrives. Binance's own documentation only says clients *should* back off on 429/418; it does not guarantee every such response carries `Retry-After`, and a header value of `0` is meaningfully different from "no header" (the former is a real, if minimal, instruction; the latter is silence).

```cpp
// NEW — the single parsing function every 429/418 call site uses; no call
// site parses Retry-After itself. Fixes round-6 P1: revision 5 clamped an
// "absurdly large" value to 1 hour and allowed the next send attempt after
// that clamp elapsed — but Binance's own 418 IP-auto-ban documentation
// describes bans that can last from 2 minutes up to 3 days depending on
// repeat-offense severity. Clamping the WAIT and then actively retrying
// early doesn't just risk another 429/418; it risks compounding an existing
// ban with additional violations while it's still active. The deadline must
// be represented exactly, not shortened for local convenience.
// FIXES ROUND-11 P0: earlier revisions named a single `deadline_ms` and
// described it as "steady-clock-relative" while §7.3.1 / RateLimitFreezePayload
// wrote it into `deadline_utc_ms`. On restart a steady-relative value
// misinterpreted as UTC either expires immediately (fail-open: ban ignored)
// or lands millennia in the future (fail-closed forever). The two time bases
// are now separate fields; substituting one for the other is a defect.
struct RetryAfterResult {
    bool present{false};        // header existed and parsed as a non-negative integer
    // Round-23 P1 — split representability (UTC add vs steady add). Either
    // false ⇒ treat as unrepresentable → permanent fence (same as 418
    // missing). A single "representable" that only checked int64 UTC left
    // steady_clock::time_point + duration overflow as UB / premature release.
    bool utc_representable{true};    // now_utc_ms + seconds*1000 fits int64
    bool steady_representable{true}; // steady_now + seconds fits TimePoint
    bool representable() const noexcept {
        return utc_representable && steady_representable;
    }
    std::int64_t retry_after_seconds{0}; // parsed duration; 0 is a valid value
    std::int64_t deadline_utc_ms{0};     // ABSOLUTE UTC; valid iff
                                          // present && utc_representable
    TimePoint deadline_steady{};          // ABSOLUTE steady; valid iff
                                          // present && steady_representable
                                          // NEVER persisted. NEVER compared to UTC.
};

inline RetryAfterResult parse_retry_after(unsigned http_status,
                                           std::string_view header_value,
                                           std::int64_t now_utc_ms,
                                           TimePoint steady_now = Clock::now()) noexcept;
```

Fail-closed defaults — **status-dependent when the header is absent/malformed (round-12 P0)**. A blanket 60-second default is incompatible with this design's own RAW_REQUESTS ~5-minute window and with Binance's documented multi-day 418 bans; "60s then retry" is a real fail-open into an active ban/exhausted bucket.

`parse_retry_after(http_status, ...)` is therefore always invoked with the HTTP status that produced it:

| HTTP status | Header present & representable | Missing / malformed |
|---|---|---|
| **418** | Honor exactly (UTC + steady pair) | **Permanent operator fence** — `RateLimitFreezePayload.source = 2`, `can_send*` latched false until operator clears; never a timed default. |
| **429** | Honor exactly | Freeze until `longest_known_ip_bucket_rollover + kMissingRetryAfterPad` (defaults: max of configured/pinned REQUEST_WEIGHT & RAW_REQUESTS windows + 120s). With no trustworthy UTC → `source=3`, `UnknownBootstrap`, wait in `conservative_wait_ms` (never forge UTC). **Active** recovery waits that window once per process start; a valid `FreezeClear` makes the epoch terminal (§7.3.1). **Never 60s.** |
| Other (should not call this) | N/A | N/A |

Additional rules:

- **Negative** — a leading `-` fails the parse (treated as missing/malformed for that status).
- **`0`**: `present == true`, both deadlines equal "now" — honored as a genuine (if minimal) instruction, distinct from missing.
- **Large but representable**: honored **exactly**, no clamp. Multi-day 418 bans stay representable when both UTC and steady adds succeed.
- **Unrepresentable** (`!representable()` — UTC **or** steady overflow): same as 418 permanent fence (cannot encode the wait → must not invent a short one).
- **Phase-B / no-clock encoding (round-21 P0 — kind first, provenance second):** when a trustworthy UTC deadline cannot be written, set header `time_kind = UnknownBootstrap`, `recorded_utc_ms = 0`, and put the wait in `conservative_wait_ms` **without changing `source`**. If this is a **merge** into an episode that already has a non-zero `deadline_utc_ms`, that UTC value is **copy-forwarded unchanged** (round-23 P0 — never zero it out just because this frame's header is UnknownBootstrap).
  - **418 missing/malformed** → still `source = 2` (permanent). Never demote to `source = 3`.
  - **418/429 with present representable Retry-After** → keep `source = 1` / `0`. `conservative_wait_ms = max(parsed_retry_after_ms, longest_known_ip_window_ms + kMissingRetryAfterPadMs)`.
  - **429 missing/malformed** → `source = 3` only; `conservative_wait_ms = longest_known_ip_window_ms + kMissingRetryAfterPadMs`.
- Never invent `deadline_utc_ms` from local wall clock (§7.0).

Every call site uses `max(scheduled_backoff_steady, result.deadline_steady)` for in-process scheduling (when a timed deadline exists), and persists freeze **kind** from the §7.3 table with provenance chosen separately. On recovery: UTC deadlines re-derive into steady after `/time`; **active** unknown-time freezes wait `conservative_wait_ms`; permanent fences restore as permanent; **valid `FreezeClear`** epochs are not active (§7.3.1).

### 7.3.1 Rate-limit freeze must survive process restart — fixes round-8 P0; types live in §10 (fixes round-9 P0 cycle)

`freeze_until(TimePoint)` is necessarily an in-process `steady_clock` wait. `steady_clock` does not survive restart; a process that crashes during an active 418 ban would come back with empty freeze state and could immediately re-send. Round 8 introduced a durable freeze record but placed its ABI in L5 — so L4 account refresh / startup baseline (which also see 429/418) reverse-depended on L5, while both files declare L5 depends on L4. That cycle is closed by §10.

**Fix — durable freeze record (§10) + recovery restore:**

1. On every 429/418 handling path (submit, reconciliation, account refresh, startup baseline — anywhere `parse_retry_after()` runs), AFTER computing both deadlines (§7.3) and BEFORE any further signed send is permitted:
   - **Freeze kind first, provenance second (round-21 P0).** Apply §7.3's status table to choose candidate `source` / timed-vs-permanent / wait fields. **Then** choose `FrameTimeKind`: published clock → `ServerCorrectedUtc` + `deadline_utc_ms`; else → `UnknownBootstrap` + `deadline_utc_ms = 0` with wait in `conservative_wait_ms` (or permanent latch). **Never** rewrite kind because the clock is missing.
   - **Single active episode (round-22 P0) + no time-semantics downgrade (round-23 P0) + wait_generation (round-31 P0).** At most one uncleared freeze epoch. If active:
     - **Do not** Ack a new watermark / allocate a new epoch.
     - **Merge** into the active `freeze_epoch`: append another `RateLimitFreeze` with the **same** epoch and merged fields. Watermark stays put.
     - Merge rules (fail-closed, monotonic — **never shorten** a prior ban):
       - If either side is `source=2` → result `source=2`.
       - Else timed `source` = worse of `{0,1,3}` with `1` ≥ `0` ≥ `3`.
       - **`deadline_utc_ms`:** write this event's UTC if present; else **copy-forward** the prior non-zero `deadline_utc_ms` (0 means "absent," not a real deadline). Never overwrite a long UTC ban with 0. Folded episode deadline = **max** across frames.
       - **`conservative_wait_ms` (per-frame contribution, round-31 clarified):** write **this event's** wait only (`0` if this 429/418 carries no wait component — e.g. deadline-only / UTC-only merge). Do **not** restate `max(old,new)` into the frame field — that would make every later merge look wait-bearing and either falsely advance `wait_generation` or fight the sink rule below. Folded episode wait = **max** across all frames' contributions (live owner and recovery both max-fold; never "latest frame alone").
       - **`wait_generation` (round-31 P0):** duration alone is **not** wait-evidence identity. If **this event's** wait contribution `> 0` (new episode or merge — **including** when folded max duration does not increase), set `wait_generation = prior_folded_wait_generation + 1` (overflow at `UINT32_MAX` → fence / refuse append). If this event's wait contribution `== 0`, copy-forward the prior folded `wait_generation` unchanged. Folded episode `wait_generation` = **max** across all frames of the epoch. A new generation **invalidates** all prior Arm/WaitSatisfied **and** any in-process `arm_ack_steady` for older gens — must re-Arm → full wait → Satisfy before probe/clear, even if the bound ms is equal or smaller.
       - Header `time_kind` on the merge frame may be `UnknownBootstrap` if the clock is unpublished **for this write**, but that does **not** authorize dropping prior UTC from the payload. Episode recovery never uses "latest frame alone."
   - **Only when no active episode:** allocate via watermark (`next=E+1` before using `E`), then `append_rate_freeze` with `wait_generation = 1` if `conservative_wait_ms > 0`, else `0`. If watermark Acked but freeze not: fence; advanced watermark stands.
   - Only after freeze Acked: timed kinds call `freeze_until` / `freeze_all_until` using `max(steady from UTC if any, conservative_wait steady if any)`; `source=2` latches until **operator** clear (§10 `FreezeClear`).
   - **Fault-injection must-pass:** prior cases plus **`utc-then-unknown-merge + restart`** → still waits the long UTC (and max conservative); **`forged-ProbeVerified + compaction + restart`** → permanent/timed still active (invalid proof rejected); **`equal-wait rematch (round-31)`**: `unknown 429 wait=120 → WaitSatisfied Ack → new unknown 429 wait=120 → old WaitSatisfied rejected → re-Arm → full wait → Satisfy` before any probe/clear; **`legacy upgrade rematch (round-32)`**: pre-`wait_generation` log `429 → Satisfy → 429 → upgrade/restart` → recovery **rejects** legacy Satisfy (no gen=0→1 promote) → migration advances gen → re-Arm → full wait → Satisfy before probe/clear.
2. On control-plane recovery (§10) / L5 `recovery_scan()`: restore **active** freeze state BEFORE any signed request — including §7.5.1 crash wait and Phase B.
   - An epoch is **cleared** only by a **valid** `FreezeClear` for that epoch (§10) — including verified `/time` proof for `ProbeVerified`.
   - **Aggregate active state (round-22/23)** — never "max epoch only" and never "latest frame only" within an epoch:
     1. Collect every epoch with a `RateLimitFreeze` and **no** valid clear.
     2. `out_uncleared_epoch_count` = that set's size. Live invariant `≤1`; if `>1`, alarm, refuse NEW epoch allocation, still restore fail-closed aggregate.
     3. For each uncleared epoch, fold **all** its live `RateLimitFreeze` frames **and** any `RateLimitFreezeSnapshot` frames: `deadline_utc_ms = max(all)`, `conservative_wait_ms = max(all)`, `wait_generation = max(all)`, `source` = worst severity (2 wins). Snapshot fields are already folded values for that write; live frames are per-event contributions. Then fold across epochs into `out_active_freeze` / `out_permanent_latch` as before.
     4. Timed still active if `deadline_utc_ms` still future after `/time` **or** (no trustworthy clear of conservative wait for the **current** `wait_generation` and) `conservative_wait_ms > 0` requiring full restart wait — take the **stricter** of the two (must satisfy both before lift).
     5. Cleared epochs are terminal per-epoch only.
   - Still-future UTC: re-derive steady only after fresh `/time`.
   - **Legacy-wait migration (round-32 P0):** an uncleared episode is **legacy-wait** iff folded `conservative_wait_ms > 0` and `max(frame.wait_generation) == 0` (all frames missing the field or explicitly 0 — pre-`wait_generation` writers). For legacy-wait:
     - **Never** set `out_has_wait_satisfied=true` from a Satisfy whose decoded `wait_generation == 0` / missing — silent `gen=0→1` promotion is forbidden (legacy bytes cannot name which wait-bearing 429 the Satisfy closed).
     - **Never** treat legacy Arm as live-session Arm evidence across this upgrade boundary.
     - Before any probe/clear under a trusted generation, durable-ize via **exactly one** seal path:
       1. **Live migration (default):** Ack same-epoch `RateLimitFreeze` with incoming wait contribution `= folded wait`, `wait_generation = 1` (prior durable max `0 + 1`). Then full `Arm → sink-measured wait → Satisfy` under gen `1`.
       2. **Compaction seal (L5 §6.1.3):** via `append_compacted_freeze_snapshot` write one aggregate with explicit `wait_generation = 1`; **drop** all legacy Arm/Satisfy (gen `0`/missing). Live process then Arm→Satisfy under gen `1`.
       3. **Sequence-proven rewrite (optional, still fail-closed):** compaction may rewrite Arm+Satisfy to explicit gen `1` **only if all** hold:
          - A durable `FreezeWaitArm` and citing `FreezeWaitSatisfied` exist for the epoch;
          - Satisfy.`arm_frame_seq` equals that Arm’s durable sequence; same epoch; bound matches folded wait;
          - **Both** `Arm.sequence_number` and `Satisfy.sequence_number` are **strictly greater** than every wait-bearing (`conservative_wait_ms > 0`) live `RateLimitFreeze` frame in the epoch (not Satisfy alone — otherwise `freeze#1 → Arm → freeze#2 → short-wait Satisfy` falsely credits pre-rematch elapsed time);
          - Emit via `append_compacted_freeze_snapshot` (gen `1`) + `append_compacted_wait_evidence` (single `CompactedFreezeWaitEvidencePayload` at gen `1`) in the **same** compaction transaction (subject to CompactionSourceBaseline tip pin — §10.1).
          If Arm is missing, unbound, or either seq is not after the last wait-bearing freeze → path (3) **illegal** — use (1) or (2) and re-wait.
     - Deep trap avoided: synthesizing **only** in-memory gen `1` while durable freeze max stays `0` lets the next wait-bearing merge also write gen `1`, so a rematch would not invalidate a wrongly promoted Satisfy — migration/compaction seal **must** be durable before probe/clear.
     - Deep trap avoided: compaction must **not** call live `append_rate_freeze` / `append_freeze_wait_*` for retained aggregates (live ABI advances gen / requires `arm_ack_steady`) — use compaction-only appends (§10).
   - **Conservative wait component (round-25/26/31/32 P0):** if folded `conservative_wait_ms > 0` and no **sink-verified** durable `FreezeWaitSatisfied` for this epoch **and current `wait_generation`** (matching bound **and** generation; legacy gen=0 Satisfy never counts):
     - Ack `append_freeze_wait_arm(...)` first (**arms** the wait; binds `wait_generation`; does not satisfy it). Sink records `arm_ack_steady` for this process session at Arm Ack.
     - Wait until the owner actor's steady clock AND the sink's own elapsed-since-Arm-Ack both meet `bound_conservative_wait_ms`.
     - Then Ack `append_freeze_wait_satisfied(...)` referencing that Arm (`arm_ordinal` + `arm_frame_seq` + **same `wait_generation`**). Sink **rejects** if `(satisfy_ack_steady - arm_ack_steady) < bound` or generation mismatch — payload duration equality alone is never enough (round-26/31).
     - **Do not** call `FreezeClear{ConservativeWaitCompleted}` when `deadline_utc_ms != 0` (still illegal).
   - If a matching `FreezeWaitSatisfied` was already Acked for the **current** `wait_generation` (prior process; sink verified then; **explicit gen > 0**), skip re-waiting the conservative component. A WaitSatisfied for an **older** generation or legacy gen `0` is **not** matching — must re-Arm.
   - **Arm without WaitSatisfied across restart (round-26):** prior Arm is **abandoned** — new process has no `arm_ack_steady` for it; must re-Arm and wait full bound again. Never accept WaitSatisfied that cites an Arm the live sink did not itself Ack in this process (unless WaitSatisfied was already durably Acked before the crash for the current **explicit** generation).
   - Permanent: restore latch until valid `FreezeClear{OperatorAuthorized}`.
3. A UTC freeze may be ignored only after a fresh `/time` proves `serverTime >= deadline_utc_ms`, persisted in `FreezeClear{ProbeVerified}`. **Dual-deadline (round-25/26/31 P0):** when folded `conservative_wait_ms > 0`, `ProbeVerified` is legal **only if** a matching sink-verified `FreezeWaitSatisfied` for that epoch **and current `wait_generation`** is already Acked — the clear **requires** that evidence present at Ack time. WaitSatisfied is **not** invalidated by a **failed** clear attempt (no separate "consumed" flag that can strand a generation mid-clear); it remains valid until the episode is successfully cleared **or** `wait_generation` advances. Binding `bound_conservative_wait_ms == folded` alone is **Rejected**. Pure unknown-time (`deadline_utc_ms == 0`) clears via `ConservativeWaitCompleted` **only after** the same generation-bound WaitSatisfied (wait>0). Lifting `source=2` requires `OperatorAuthorized` only.
4. **The freeze-verification `/time` probe needs its own self-contained, persisted, rate-limited credit — not the ordinary weight trackers — fixes this round's P0 (round 14's fix reintroduced the exact bootstrap circularity §7.0 already exists to solve, and had no closed-loop attempt/backoff discipline).** Round 14 exempted the probe from `is_frozen()` but left it going through `try_reserve_weight_only()`'s ordinary reservation arithmetic — which itself needs `server_now_ms` (derived from the clock offset this probe's whole job is to help establish/confirm) and a **configured** `RequestWeightTrackerSet` (populated from `exchangeInfo`, which may not have been fetched yet on a cold restart straight into an active ban — exactly Phase B's §7.0 circularity, reintroduced here for the freeze-specific case). Left unaddressed, an implementation could self-lock again (probe can't reserve because the tracker it needs isn't configured yet) or, if "periodically re-check" were read loosely, send `/time` at unbounded frequency during an active ban — worsening it, per Binance's own repeat-offense escalation for 418.

   **Fix — `FreezeProbeCredit`: a dedicated, fixed-capacity reservation, independent of `RequestWeightTrackerSet`/`RawRequestsTrackerSet` and of `exchangeInfo` configuration state, with an actual durable record type backing its attempt tracking (fixes this round's P0 — round 15's version claimed durability with no backing `DurableRecordType`/payload/recovery algorithm, and hardcoded a weight credit that could undercut an operator-configured higher weight):**

   ```cpp
   // NEW — held OUTSIDE the normal weight/order-count trackers entirely,
   // reserved unconditionally at process construction (before Phase B,
   // before exchangeInfo, before a clock offset exists) so it can never be
   // blocked by, or compete with, ordinary traffic's configuration state.
   // Integer-table backoff (no std::pow, no floating point) — fixes this
   // round's P0: std::pow(double, double) is neither exact nor guaranteed
   // deterministic across platforms for this kind of scheduling arithmetic,
   // and `steady_now_plus_ms()` was referenced with no definition; both
   // replaced with an explicit, portable integer table.
   // Declared BEFORE FreezeProbeCredit (fixes an ordering issue found this
   // round while re-reading this block end-to-end: release_probe() below
   // calls this, but it was originally defined AFTER the struct that uses
   // it — a free function can't be called from an earlier member function
   // definition in the same translation unit without being declared first).
   // Fixes this round's P1: TimePoint's own operator+ has no overflow
   // check; this wraps it with an explicit saturating clamp to
   // TimePoint::max() rather than relying on well-defined behavior at a
   // boundary the standard doesn't actually guarantee.
   inline TimePoint add_ms_saturating(TimePoint t, std::int64_t ms) noexcept {
       auto remaining = TimePoint::max() - t;
       auto add = std::chrono::milliseconds(ms);
       if (add >= remaining) return TimePoint::max();
       return t + add;
   }

   struct FreezeProbeCredit {
       static constexpr std::uint32_t kRawCredit = 1;
       static constexpr std::uint32_t kMaxTotalAttempts = 8;  // hard cap, PER
                                                                 // freeze_epoch
                                                                 // — see below
       // Explicit backoff table (ms), indexed by attempt ordinal - 1,
       // clamped to the last entry — replaces std::pow/steady_now_plus_ms():
       static constexpr std::int64_t kBackoffTableMs[kMaxTotalAttempts] = {
           2000, 4000, 8000, 16000, 32000, 64000, 128000, 300000};

       // Weight credit — fixes this round's P0 ("never guess downward"):
       // computed at reservation time, never a bare compile-time constant,
       // as max(kPinnedEndpointWeight[GetServerTime], the recovered
       // operator-configured EndpointWeightConfig's GetServerTime weight IF
       // one has already been durably recovered by the time this runs —
       // §10's out_has_weights, revision 14, is exactly what lets this be
       // "if known" rather than a hard dependency). This credit is never
       // consumed FROM RequestWeightTrackerSet (still fully independent,
       // per round 14/15) — it only sizes FreezeProbeCredit's own local
       // slot, so the local bookkeeping for what the probe actually costs
       // is never smaller than what Binance will actually charge for it.
       static std::uint32_t weight_credit(const EndpointWeightConfig* recovered_cfg) noexcept {
           std::uint32_t pinned = kPinnedEndpointWeight[static_cast<std::size_t>(
               PrivateRestEndpoint::GetServerTime)];
           if (!recovered_cfg) return pinned;
           return std::max(pinned, recovered_cfg->weights[static_cast<std::size_t>(
               PrivateRestEndpoint::GetServerTime)]);
       }

       // Epoch-scoped state. Round-25 P0: MUST call restore_from_recovery()
       // after recover_control_plane() before any try_reserve_probe — otherwise
       // active_epoch==0 and the first probe on a recovered epoch looks "new"
       // and zeroes attempts_made (crash-loop launders the 8-cap).
       // Round-26 P1: epoch changes ONLY via restore_from_recovery or
       // bind_new_episode_after_durable_create — never inside try_reserve_probe.
       std::uint32_t active_epoch{0};
       bool in_flight{false};
       bool credit_restored{false};     // gates try_reserve_probe
       std::uint32_t attempts_made{0};  // == max durable Acked attempt_ordinal
       std::uint32_t reserved_ordinal{0}; // set by try_reserve; 0 = none
       std::uint64_t reserved_or_last_attempt_seq{0}; // Ack sequence binding
       TimePoint next_allowed_attempt_steady{};
       // Round-27 P0: absolute UTC not-before for the NEXT FreezeProbeCredit
       // send. MUST be max(backoff_deadline, folded_deadline_utc_ms) whenever
       // a folded UTC deadline exists — never backoff-alone. 0 only when both
       // backoff schedule and deadline are absent (pure unknown-time).
       std::int64_t not_before_utc_ms{0};

       // Round-27 P0/P1 — compute durable not-before. false → unrepresentable;
       // caller MUST refuse the probe append/send and fence (fail-closed).
       static bool compute_not_before_utc_ms(
           std::optional<std::int64_t> now_utc_ms,
           std::uint32_t attempt_ordinal_1based,
           std::int64_t folded_deadline_utc_ms,
           std::int64_t& out_not_before) noexcept {
           out_not_before = 0;
           std::int64_t backoff_gate = 0;
           if (now_utc_ms) {
               std::size_t idx = attempt_ordinal_1based == 0
                                     ? 0
                                     : static_cast<std::size_t>(attempt_ordinal_1based - 1);
               if (idx >= kMaxTotalAttempts) idx = kMaxTotalAttempts - 1;
               if (!checked_add_i64(*now_utc_ms, kBackoffTableMs[idx], backoff_gate))
                   return false;  // overflow — refuse / fence
               out_not_before = backoff_gate;
           }
           // Deadline fold: even without now_utc, a known absolute deadline
           // still arms the UTC gate (restore/try_reserve will refuse while
           // now_utc is absent — fail-closed, never "skip gate").
           if (folded_deadline_utc_ms > out_not_before)
               out_not_before = folded_deadline_utc_ms;
           return true;
       }

       // Inject durable attempt state + folded freeze deadline.
       // epoch==0 ⇒ no active freeze (idle credit).
       // Round-26/27: NEVER clear backoff/deadline gates unconditionally.
       void restore_from_recovery(
           std::uint32_t epoch,
           std::span<const FreezeProbeAttemptPayload> attempts,
           std::int64_t folded_deadline_utc_ms,  // from out_active_freeze; 0=absent
           TimePoint steady_now,
           std::optional<std::int64_t> now_utc_ms) noexcept {
           credit_restored = true;
           in_flight = false;
           reserved_ordinal = 0;
           reserved_or_last_attempt_seq = 0;
           active_epoch = epoch;
           attempts_made = 0;
           not_before_utc_ms = 0;
           next_allowed_attempt_steady = {};
           if (epoch == 0) return;
           for (const auto& a : attempts) {
               if (a.freeze_epoch != epoch) continue;
               if (a.attempt_ordinal > attempts_made)
                   attempts_made = a.attempt_ordinal;
               if (a.not_before_utc_ms > not_before_utc_ms)
                   not_before_utc_ms = a.not_before_utc_ms;
           }
           if (attempts_made > kMaxTotalAttempts)
               attempts_made = kMaxTotalAttempts;
           // Round-27 P0: fold the active freeze's UTC deadline into the gate
           // even when no attempts exist yet (fresh episode / crash before
           // first Ack) — otherwise a multi-day deadline is invisible to
           // credit and crash-loops burn the 8-cap on short backoffs alone.
           if (folded_deadline_utc_ms > not_before_utc_ms)
               not_before_utc_ms = folded_deadline_utc_ms;
           // Steady backoff from last attempt (UnknownBootstrap / no UTC).
           if (attempts_made > 0 && attempts_made <= kMaxTotalAttempts) {
               std::size_t idx = attempts_made - 1;
               next_allowed_attempt_steady =
                   add_ms_saturating(steady_now, kBackoffTableMs[idx]);
           }
           // Round-28 P0: when now_utc is absent and not_before > 0, do NOT
           // invent a free TimeResyncCredit escape and do NOT clear the gate.
           // Steady hold from last attempt (above) still applies; the next
           // FreezeProbeCredit send (if any budget remains) may re-publish
           // clock under Ack-before-send, burning a durable ordinal.
           if (now_utc_ms && not_before_utc_ms > 0 &&
               *now_utc_ms < not_before_utc_ms) {
               std::int64_t remain = 0;
               if (checked_sub_i64(not_before_utc_ms, *now_utc_ms, remain) &&
                   remain > 0) {
                   TimePoint utc_gate = add_ms_saturating(steady_now, remain);
                   if (utc_gate > next_allowed_attempt_steady)
                       next_allowed_attempt_steady = utc_gate;
               } else {
                   // Unrepresentable remain → hold at TimePoint::max (fail-closed).
                   next_allowed_attempt_steady = TimePoint::max();
               }
           }
       }

       // Round-26 P1 — ONLY after NEW episode's watermark + RateLimitFreeze
       // are both Acked. MUST NOT be called merely because try_reserve saw
       // a different epoch.
       void bind_new_episode_after_durable_create(
           std::uint32_t epoch,
           std::int64_t folded_deadline_utc_ms) noexcept {
           if (!credit_restored) return;
           active_epoch = epoch;
           attempts_made = 0;
           reserved_ordinal = 0;
           reserved_or_last_attempt_seq = 0;
           in_flight = false;
           not_before_utc_ms = folded_deadline_utc_ms > 0 ? folded_deadline_utc_ms : 0;
           next_allowed_attempt_steady = {};
       }

       // Round-27 — call after same-epoch merge raises deadline_utc_ms.
       void on_active_freeze_deadline_raised(std::int64_t folded_deadline_utc_ms) noexcept {
           if (folded_deadline_utc_ms > not_before_utc_ms)
               not_before_utc_ms = folded_deadline_utc_ms;
       }

       // Round-26/28/29 P0 — returns reserved 1-based ordinal on success; 0 on refuse.
       // Does NOT consume durable budget until append_freeze_probe_attempt Ack.
       // UTC gate (round-27/28):
       //  - not_before > 0 && now_utc present && now < not_before → refuse
       //  - not_before > 0 && now_utc absent → allow (ClockRepublishOrVerify;
       //    burns durable ordinal; may clear on response if deadline past — round-29)
       // Wait gate (round-29, DOWNGRADED round-30/31): wait_ok is caller-computed
       // (folded wait==0 || FreezeWaitSatisfied Acked for CURRENT wait_generation)
       // and is ONLY a fast-path — it saves a doomed reserve+append round-trip
       // when the caller already knows the wait is pending. It is NOT the
       // authoritative gate and MUST NOT be trusted as one: a caller bug that
       // passes wait_ok=true prematurely (including stale-generation Satisfy)
       // still reserves in-process, but the AUTHORITATIVE check lives sink-side
       // in DurableControlPlaneSink::append_freeze_probe_attempt() (round-30/31),
       // which independently re-verifies bound+generation against durably-Acked
       // state before Ack. See §7.3.1 linearization step 1/5.
       std::uint32_t try_reserve_probe(std::uint32_t freeze_epoch,
                                       TimePoint steady_now,
                                       std::optional<std::int64_t> now_utc_ms,
                                       bool wait_ok) noexcept {
           if (!credit_restored) return 0;
           if (freeze_epoch == 0 || freeze_epoch != active_epoch) return 0;
           if (!wait_ok) return 0;  // Round-29 fast-path only — see comment above;
                                     // round-30 sink-side Ack gate is authoritative
           if (in_flight) return 0;
           if (attempts_made >= kMaxTotalAttempts) return 0;
           if (steady_now < next_allowed_attempt_steady) return 0;
           if (not_before_utc_ms > 0 && now_utc_ms &&
               *now_utc_ms < not_before_utc_ms) return 0;
           reserved_ordinal = attempts_made + 1;
           in_flight = true;
           return reserved_ordinal;
       }

       // Call if append_freeze_probe_attempt failed / was not Acked — frees
       // the in-flight reservation WITHOUT burning an attempt ordinal.
       void cancel_reservation() noexcept {
           in_flight = false;
           reserved_ordinal = 0;
       }

       // Call ONLY after append_freeze_probe_attempt(...).acked().
       // Binds AuditAppendResult::sequence (round-27 P1). Advances attempts_made.
       // Round-26 P0: the /time NETWORK SEND is illegal until this returns true.
       // NOTE: sequence may be 0 (new-generation genesis) — that is a valid
       // Acked frame id; do NOT treat 0 as "missing."
       bool confirm_attempt_acked(std::uint32_t ordinal,
                                  const AuditAppendResult& append_result,
                                  std::int64_t attempt_not_before_utc_ms) noexcept {
           if (!in_flight || ordinal == 0 || ordinal != reserved_ordinal) return false;
           if (ordinal != attempts_made + 1) return false;
           if (!append_result.acked()) return false;
           attempts_made = ordinal;
           reserved_or_last_attempt_seq = append_result.sequence;
           if (attempt_not_before_utc_ms > not_before_utc_ms)
               not_before_utc_ms = attempt_not_before_utc_ms;
           reserved_ordinal = 0;
           return true;
       }

       // CHANGED — fixes this round's P0 (permanent self-lock on any timed
       // UTC freeze longer than the backoff table's ~9-minute span): the old
       // signature always scheduled the next attempt via the blind backoff
       // table alone (2s -> 300s), so a multi-hour/multi-day 418 ban would
       // exhaust all 8 attempts purely from elapsed real time, long before
       // the deadline could plausibly have passed — after which
       // try_reserve_probe() refuses forever (attempts_made stays at 8,
       // nothing ever resets it short of a clear that can now never happen).
       // Fixed: the caller passes the folded episode's deadline (converted
       // to an estimated steady_clock time via the current best offset,
       // present whenever deadline_utc_ms > 0; std::nullopt for a pure
       // unknown-time episode with no UTC anchor) and the next attempt is
       // never scheduled earlier than that estimate. This makes
       // kMaxTotalAttempts bound genuine RETRY failures occurring near/after
       // the deadline, not the wait FOR the deadline itself.
       // Round-26: attempts_made is advanced in confirm_attempt_acked (durable
       // Ack), NOT here — release_probe only clears in_flight and schedules
       // the next steady backoff. Double-counting on release is forbidden.
       // Round-27 P0: durable not_before already carries max(backoff, deadline)
       // from the Acked attempt payload; release's steady max is the in-process
       // mirror only — it must NOT be the sole carrier of the deadline gate.
       // After FreezeClear Ack, also call note_episode_cleared() so a stale
       // active_epoch cannot keep accepting reserves for a resolved episode.
       void release_probe(bool network_ok, bool freeze_clear_acked,
                          TimePoint steady_now,
                          std::optional<TimePoint> deadline_steady_estimate) noexcept {
           in_flight = false;
           reserved_ordinal = 0;
           if (freeze_clear_acked) {
               note_episode_cleared();
               return;
           }
           (void)network_ok;
           if (attempts_made == 0) return;
           std::size_t idx = attempts_made <= kMaxTotalAttempts
                                 ? attempts_made - 1
                                 : kMaxTotalAttempts - 1;
           TimePoint local_backoff = add_ms_saturating(steady_now, kBackoffTableMs[idx]);
           next_allowed_attempt_steady = deadline_steady_estimate
               ? std::max(local_backoff, *deadline_steady_estimate)
               : local_backoff;
       }

       // Round-26 — after terminal FreezeClear Ack (any kind). Idles credit
       // so try_reserve_probe cannot keep burning budget against a cleared epoch.
       void note_episode_cleared() noexcept {
           active_epoch = 0;
           attempts_made = 0;
           reserved_ordinal = 0;
           reserved_or_last_attempt_seq = 0;
           in_flight = false;
           not_before_utc_ms = 0;
           next_allowed_attempt_steady = {};
       }
   };
   ```

   - **Durable attempt + Ack-before-send (round-26/27/28/29/30 P0):** `FreezeProbeAttempt` via `append_freeze_probe_attempt()`. Linearization — **the sole freeze-period `/time` path**:
     1. **Wait gate, fast-path only (round-29, downgraded round-30/31):** if folded `conservative_wait_ms > 0` and no sink-verified `FreezeWaitSatisfied` for this epoch **and current `wait_generation`** → **refuse** `try_reserve_probe` locally (complete Arm→Satisfy first; no network). Caller-computed; **not trusted** — see step 5.
     2. `ordinal = try_reserve_probe(epoch, …)` — must be >0.
     3. Choose `purpose`: `ClockRepublishOrVerify` iff trustworthy `now_utc` is absent; else `DeadlineOrVerify`. Build `not_before_utc_ms` via `compute_not_before_utc_ms(...)` — **`max(checked now+backoff, folded_deadline)`**. If compute returns false → `cancel_reservation()`, fence, no send.
     4. Payload `{freeze_epoch, attempt_ordinal=ordinal, cleared=false, purpose, not_before_utc_ms=out}`.
     5. **`append_freeze_probe_attempt(...).acked()` REQUIRED** — sink independently re-verifies `folded wait == 0 || matching sink-verified FreezeWaitSatisfied already Acked for CURRENT wait_generation` (round-30/31 P0; AUTHORITATIVE). Reject on mismatch (including stale gen) → `Failed`, no `.sequence`. On Ack → `confirm_attempt_acked(ordinal, result, not_before)`.
     6. **Only then** may the process send `GET /api/v3/time`.
     7. On response (round-29 P0 — **both purposes may clear**):
        - Publish `ClockOffsetPublisher` if the response is usable.
        - Build `FreezeTimeProbeProof` (MAC/host/nonce/epoch/deadline bind) from this response.
        - If `source != 2` and `serverTime >= folded deadline_utc_ms` and proof verifies and (wait==0 or current-gen WaitSatisfied already Acked): **`append_freeze_clear{ProbeVerified}`** from **this same `/time`** — then `release_probe(..., freeze_clear_acked=true)`. Applies equally to `DeadlineOrVerify` and `ClockRepublishOrVerify` (renamed from `ClockRepublish`; the old "republish must never clear" rule is **withdrawn** — it caused last-ordinal self-lock).
        - Else if `serverTime < folded deadline` (or proof/WaitSatisfied incomplete / stale gen): **do not** clear; hold until `not_before` / complete current-gen WaitSatisfied; ordinal remains burned.
        - Transport/publish failure → keep freeze/fence; ordinal already burned (fail-closed).
     8. If step 5 fails (Ack rejected — including wait-gate / stale-generation mismatch) → `cancel_reservation()`; no send; budget unchanged. **Fault-injection must-pass (round-30/31):** `wait > 0 && (no WaitSatisfied OR only stale-gen WaitSatisfied) && caller's local wait_ok wrongly true → try_reserve_probe reserves → append_freeze_probe_attempt rejects → cancel_reservation() → zero /time → attempts_made unchanged`.
     9. Kill after step 5 / before step 6: durable ordinal + not_before stand. Kill before step 5: may retry same ordinal.
   - **UTC gate / clock re-publish while frozen (round-27/28/29 P0):**
     - If `not_before_utc_ms > 0` and trustworthy `now_utc` is present and `now_utc < not_before` → `try_reserve_probe` **refuses** (no early poll toward a long deadline).
     - If `not_before_utc_ms > 0` and `now_utc` is **absent**: **do not** open `TimeResyncCredit`. Use `FreezeProbeAttempt` with `purpose=ClockRepublishOrVerify` (burns an ordinal under the 8-cap). After response: if clear preconditions hold → `ProbeVerified` clear (round-29); else hold until not_before.
     - While any uncleared freeze epoch is active, `TimeResyncCredit` / §7.5.1 first-packet `/time` **MUST NOT** send (§7.1 round-28). Single recoverable upper bound per frozen episode = `FreezeProbeCredit::kMaxTotalAttempts` (8).
     - Exhaustion of the 8-cap with freeze still active → alarm + hold. Permanent (`source=2`) needs `OperatorAuthorized`. Timed: if a prior Acked `/time` already proved `serverTime >= deadline` but clear failed only for missing WaitSatisfied, complete WaitSatisfied then clear using that proof **without a new network send** only if the proof was retained in the durable `FreezeClear` attempt path — otherwise the round-29 wait-before-probe rule ensures WaitSatisfied precedes the last ordinal. Must-pass: `attempts=7 → crash loses clock → ordinal 8 ClockRepublishOrVerify → serverTime >= deadline (+ WaitSatisfied if required) → FreezeClear Ack → restart not frozen`.
   - **Non-terminal wait evidence (round-25/26/31 P0):** `FreezeWaitArm` then `FreezeWaitSatisfied`:
     - Arm first (`append_freeze_wait_arm`); payload binds `wait_generation == folded`; sink stores `arm_ack_steady` for this process session.
     - After full bound elapses, `append_freeze_wait_satisfied` citing that Arm (`arm_ordinal` + `arm_frame_seq` + **same `wait_generation`**); sink rejects unless `(satisfy_ack_steady - arm_ack_steady) >= bound` AND binds match folded wait **and generation**. **Does not** lift the freeze.
     - Merge that writes a new `RateLimitFreeze` with **incoming wait contribution `> 0`** **advances `wait_generation`** (round-31) — prior WaitSatisfied is insufficient even if bound ms is equal/smaller; new Arm+Satisfy required before ProbeVerified; in-process `arm_ack_steady` for older gens is invalidated on that Ack.
     - Merge that only raises / copy-forwards `deadline_utc_ms` with **incoming wait contribution `== 0`** copy-forwards `wait_generation` (does not invalidate wait evidence).
     - Merge that raises `deadline_utc_ms` calls `on_active_freeze_deadline_raised` (and any in-flight schedule must re-Ack a new attempt not_before if probing).
     - Compaction retains Arm+WaitSatisfied **only for the current folded `wait_generation`** of each uncleared epoch (drop stale generations).
   - **Terminal clear (round-22/25/26/29/31):** `FreezeClear` + `append_freeze_clear`:
     - `ProbeVerified` — `/time` proof as before; **illegal** if `source=2`. Legal after **either** `DeadlineOrVerify` or `ClockRepublishOrVerify` when `serverTime >= folded deadline` and proof verifies; when folded `conservative_wait_ms > 0`, sink **requires** matching WaitSatisfied for **current `wait_generation`**. When `conservative_wait_ms == 0`, WaitSatisfied is not required.
     - `ConservativeWaitCompleted` — pure unknown-time only (`deadline_utc_ms == 0`); illegal for `source=2` and illegal whenever UTC deadline is active; when `conservative_wait_ms > 0`, **same generation-bound WaitSatisfied requirement**.
     - `OperatorAuthorized` — only legal clear for `source=2`.
     Ack clear **before** in-memory lift; `release_probe(..., freeze_clear_acked=true)` only after that Ack.
   - **Recovery reconstruction:** epoch not active iff valid terminal `FreezeClear`. After `recover_control_plane()`, caller **MUST** `credit.restore_from_recovery(binding_epoch, attempts, out_active_freeze.deadline_utc_ms, steady_now, now_utc)` before any probe (round-25/27). `attempts_made` = max durable ordinal; `not_before` = max(attempt not_befores, folded deadline) — crash restart must not refill the 8-cap or forget a multi-day deadline.
   - **Hard cap / backoff / scope:** unchanged (8 attempts, integer table, `/time` only for FreezeProbeCredit).

### 7.4 Endpoint request-weight table + pre-send reservation for EVERY REST call — fixes round-8 P0 + round-9 P1

Round 8 closed the reservation gap in prose but left `endpoint_request_weight()` as a bare declaration — every call site depends on it, so an implementation would fail to compile or invent weights. Round 9 pins concrete values (Binance spot REST docs as of this revision) and a versioned override path.

```cpp
enum class PrivateRestEndpoint : std::uint8_t {
    PostOrder = 0,           // POST /api/v3/order
    GetOrder = 1,            // GET  /api/v3/order
    GetAccount = 2,          // GET  /api/v3/account
    GetRateLimitOrder = 3,   // GET  /api/v3/rateLimit/order
    GetExchangeInfo = 4,     // GET  /api/v3/exchangeInfo
    GetServerTime = 5,       // GET  /api/v3/time
};

// PINNED defaults — Binance spot REST documented weights (IP).
// PostOrder=1, GetOrder=4, GetAccount=20, GetRateLimitOrder=40,
// GetExchangeInfo=20, GetServerTime=1. Unknown → UINT32_MAX (refuse).
inline constexpr std::uint32_t kPinnedEndpointWeight[] = {
    /*PostOrder*/ 1u, /*GetOrder*/ 4u, /*GetAccount*/ 20u,
    /*GetRateLimitOrder*/ 40u, /*GetExchangeInfo*/ 20u, /*GetServerTime*/ 1u,
};
static_assert(sizeof(kPinnedEndpointWeight) / sizeof(kPinnedEndpointWeight[0]) == 6);

// Versioned runtime table: starts as kPinnedEndpointWeight; may be replaced
// only by an operator-loaded, checksummed EndpointWeightConfig (durable
// control-plane metadata, §10) whose config_version is audited. Online
// Binance weight changes are NOT auto-scraped from undocumented fields —
// an unexpected weight observed in practice (e.g. via 429 patterns) is an
// operator incident, not a silent self-edit. Safety margin: effective
// reservation uses weight + kEndpointWeightSafetyPad (default 0 for pinned
// exact values; operator config may raise pad). Never guess downward.
struct EndpointWeightConfig {
    std::uint32_t config_version{0};
    std::uint32_t weights[6]{};
    std::uint32_t safety_pad{0};
};

inline std::uint32_t endpoint_request_weight(PrivateRestEndpoint ep,
                                              const EndpointWeightConfig& cfg) noexcept;

// Weight + RAW_REQUESTS (+1) for non-order-placing calls. Does NOT touch ORDERS.
inline bool try_reserve_weight_only(RequestWeightTrackerSet& weight,
                                     RawRequestsTrackerSet& raw,
                                     PrivateRestEndpoint ep,
                                     const EndpointWeightConfig& cfg,
                                     std::int64_t server_now_ms,
                                     TimePoint steady_now = Clock::now()) noexcept {
    const auto w = endpoint_request_weight(ep, cfg);
    if (!weight.try_reserve(w, server_now_ms, steady_now)) return false;
    if (!raw.try_reserve(1, server_now_ms, steady_now)) {
        weight.rollback(w, server_now_ms, steady_now);
        return false;
    }
    return true;
}
```

**Mandatory call sites:**

| Call site | Reservation |
|---|---|
| `POST /api/v3/order` (the only order-placing site — no `-1021` re-POST exists, L5 §4.5) | `try_reserve_all_budgets` — weight + ORDERS + RAW_REQUESTS |
| `GET /api/v3/order` | `try_reserve_weight_only(..., GetOrder)` |
| `GET /api/v3/account` | `try_reserve_weight_only(..., GetAccount)` |
| `GET /api/v3/rateLimit/order` | `try_reserve_weight_only(..., GetRateLimitOrder)` |
| `GET /api/v3/exchangeInfo` | `try_reserve_weight_only(..., GetExchangeInfo)` |
| `GET /api/v3/time` | `try_reserve_weight_only(..., GetServerTime)` |

If reservation fails: do not send. Startup-baseline / clock-sync paths fail closed rather than "send anyway."

### 7.5 `RAW_REQUESTS` tracking — fixes round-9 P0 (was explicitly deferred → real 429 risk)

Binance's `exchangeInfo.rateLimits[]` includes `rateLimitType=RAW_REQUESTS` (typically a multi-minute interval with a high count). Round 8's "leave it for a future revision" left a live fail-open: a client respecting only REQUEST_WEIGHT + ORDERS can still trip RAW_REQUESTS and draw 429/418.

```cpp
// Round-12 P1: REQUEST_WEIGHT and RAW_REQUESTS may each expose MULTIPLE
// intervals in exchangeInfo (same shape as ORDERS). A single
// RequestWeightTracker / RawRequestsTracker instance cannot gate them.
// Both are modeled as fixed-capacity interval sets, reusing the
// OrderCountTrackerSet pattern (kMaxIntervals = 8, empty set → fail closed,
// BucketIdentity correction rule, late rotation per slot).
class RequestWeightTrackerSet { /* parallel to OrderCountTrackerSet;
    CorrectionMode::MaxOfLocalAndServer; try_reserve / rollback */ };
class RawRequestsTrackerSet { /* same; +1 per HTTP request */ };
// Legacy single-window RequestWeightTracker remains only as the per-slot
// engine inside these sets (and for unit tests of one window).
```

Every REST send path that calls `try_reserve_weight_only` / `try_reserve_all_budgets` reserves against **all** active REQUEST_WEIGHT and RAW_REQUESTS intervals. 429/418 freezes every slot in both sets alongside ORDERS.

#### 7.5.1 Restart baseline for RAW_REQUESTS / REQUEST_WEIGHT — fixes round-10/11/12 P0

`GET /api/v3/rateLimit/order` solves ORDERS only. IP-scoped buckets have no usage-report endpoint. Two recovery paths:

1. **Clean shutdown (snapshot present and still-current)**: orderly shutdown appends `RateLimitUsageSnapshot` (§10). After Phase B's `/time` confirms the snapshot's bucket is still current, seed `used := snapshot.used + kRestartPad` and continue. If `/time` shows the bucket already rolled, treat as crash-equivalent for any remaining wait.
2. **Crash / unknown usage — fixes round-12 P0 (no wall-clock dependency):** wait `kCrashBootstrapWait` of **steady_clock** time from process start (`max_pinned_ip_window + pad`, default 420s). **No local-UTC boundary math, no assumption about wall-clock skew.** Zero network I/O during the wait. Then Phase B's `/time` is the first packet.
3. Recovered durable freezes (§7.3.1) apply first and dominate. First-ever start skips the wait (§7.0 rule 0).

Fault-injection: kill -9 in a hot bucket → zero packets for the full steady wait even if wall clock is stepped +1 hour at restart; clean stop → snapshot resume.

## 8. Security contract for this client — distinct from `binance_rest_snapshot.hpp`

Fixes: the original draft said "reuse `binance_rest_snapshot.hpp`'s pattern," which conflated *protocol shape* with *security guarantees*. Correct framing: **reuse the Boost.Beast plumbing structure (resolve → connect → TLS handshake → write → read → parse), do not assume its security posture carries over.** `binance_rest_snapshot.hpp` talks to a public, unauthenticated endpoint; a client carrying signed requests and (indirectly, via headers) API keys needs its own, independently specified and tested contract:

- **Explicit hostname verification and TLS policy — fixes round-7 P1 (previously stated as a bare bullet with no concrete minimum-version/CA-store/cross-platform contract).** SNI + certificate CN/SAN match against the bound environment's `base_host`, plus:
  - **Minimum TLS version 1.2**, explicitly configured on the `ssl::context` (`ssl::context::tlsv12_client` at minimum, matching `binance_rest_snapshot.hpp`'s existing choice — not left at OpenSSL's/the platform's own default, which can vary by OpenSSL build and has historically allowed weaker versions unless explicitly restricted).
  - **Certificate verification is always `ssl::verify_peer` with `set_default_verify_paths()`** (matching `binance_rest_snapshot.hpp`'s existing pattern) — never `verify_none`, and never configurable to disable verification via `PrivateRestConfig` or any other runtime knob; this must be a compile-time property of the class, not something a bad config value could silently turn off in production.
  - **Windows/MSVC CA store policy, explicit rather than assumed**: `set_default_verify_paths()`'s behavior differs meaningfully between the GCC 14/Linux production target and the MSVC 19.51/Windows local-dev target (`CLAUDE.md`'s stated dual-compiler setup) — on Linux it typically resolves to the system's OpenSSL CA bundle; on Windows, the OpenSSL build in use may or may not automatically bridge to the Windows Certificate Store, and a build that silently falls back to "no CA bundle found, verification effectively can't succeed against anything" would be a fail-*open*-shaped bug if it's ever misread as "verification passed" rather than "connection failed." This client explicitly verifies, at `init()` time (a one-time startup check, not per-request), that the configured verify paths actually resolve to a non-empty, loadable CA bundle on the current platform — failing `init()` outright (never silently proceeding with an empty trust store) if they don't. This is a genuine cross-platform gap this design must not leave to "probably works the same as the existing public snapshot client," since that client has never been tested against a production TLS trust-store failure mode the way a client carrying signed, authenticated requests needs to be.
- **Per-phase deadlines with real cancellation, not just a timer that expires — fixes round-7 P1.** Separate budgets for DNS resolve, TCP connect, TLS handshake, write, and read (`SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` §2's `PrivateRestConfig`) — a slow DNS resolver shouldn't be able to consume the entire connect budget and leave zero time for the actual connect. Round 7 found this needs to be stated more precisely: **`beast::tcp_stream`'s own `expires_after()` timeout mechanism does not, by itself, guarantee a bounded DNS resolve phase** — `tcp::resolver::resolve()` (a separate Asio object from `tcp_stream`) has its own cancellation surface, and a resolver call left to run to the OS resolver's own (often much longer, or even unbounded on some platforms/misconfigurations) timeout would defeat the resolve-phase budget entirely, no matter how the stream's own timer is configured. **Fix**: the resolver's async form is used with an explicit, independently-armed timer (a `net::steady_timer` or the resolver's own cancellation token) covering exactly the resolve budget — on expiry, `resolver.cancel()` is called explicitly, and the resolve is treated as a `Timeout` outcome, structurally identical to how the other phases already time out via `tcp_stream::expires_after()`. This closes the one phase where "per-phase deadline" was previously an assumption about `tcp_stream` rather than a property actually enforced for that specific phase.
- **Response-size cap enforcement — fixes round-5 P1, extended by round-6 P1 (round 5's fix covered the body; round 6 found the same class of gap in three adjacent places).** `transport_policy.hpp::check_response_size()` exists, but the actual read pattern this spec proposed reusing (`binance_rest_snapshot.hpp`'s `http::read(stream, buffer, res)` with an unconfigured `http::response<http::string_body>`) buffers the **entire** response body into memory — allocating for it — before any code has a chance to call `check_response_size()` at all. **Fix (round 5, still correct)**: configure `http::response_parser<http::string_body>::body_limit(p.max_response_bytes)` *before* the read call, so Beast itself rejects an oversized body mid-stream. `check_response_size()` is retained as a defense-in-depth assertion after a successful read, not as the primary enforcement mechanism. **Round 6 found `body_limit()` alone is not the whole enforcement surface**:
  - **Header size**: `http::response_parser` also has a separate header-size limit (`header_limit()`, defaulting to a much larger value than this client should accept) — a response with an oversized header section (many/huge header fields) can consume unbounded buffer space before the body-parsing phase, and before `body_limit()` has any effect at all. `header_limit()` is configured explicitly (a small, fixed bound — Binance's real responses have a handful of short, well-known headers; anything far beyond that is itself a signal something is wrong) alongside `body_limit()`, not left at Beast's default.
  - **No automatic decompression, or a bounded one**: if this client (or a proxy in front of it) ever negotiates `Content-Encoding` (e.g. via an `Accept-Encoding` header this client sends), a compressed response's *decompressed* size is not bounded by `body_limit()`, which caps the wire bytes, not the inflated output — a small compressed payload could decompress to something far larger (a decompression-bomb shape). **Fix**: this client never sends an `Accept-Encoding` header that would negotiate compression in the first place (Binance's REST API does not require it), removing the decompression path entirely rather than needing to bound it.
  - **Force-close after any parse exception or unexpected EOF**: an exception from `body_limit()`'s rejection, `header_limit()`'s rejection, an unexpected connection close mid-response, or any simdjson parse failure all leave the underlying stream in an undefined protocol state (partial bytes consumed, unknown framing position) — per the per-phase-deadline rule (`SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` §2), **any such failure force-closes the connection**; it is never reused for a subsequent request, for the same reason a timed-out connection isn't (§2 of the L5 spec).
- **Exception safety — fixes round-5 P1 (a real correctness gap, not just a style note).** `SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` §2 declares `submit_order()`/`query_order()` `noexcept`, but the machinery underneath (Boost.Beast, Boost.Asio, OpenSSL, simdjson) can and does throw — `boost::system::system_error`, `std::bad_alloc` (including from the body-limit rejection above), `simdjson::simdjson_error`. An uncaught exception escaping a `noexcept` function calls `std::terminate()` — for an L5 execution path, that's an uncontrolled process crash, exactly the kind of instability this whole spec's fail-closed philosophy exists to avoid, not a graceful `NetworkError`. `binance_rest_snapshot.hpp` (existing code) already gets this right — its entire fetch body is wrapped in `try { ... } catch (...) { return std::nullopt; }`. Both `submit_order()` and `query_order()`'s real implementations must wrap their entire network+parse body identically: **any exception, of any type, caught and converted to `SubmitOutcome::NetworkError` / `ReconcileQueryOutcome::Inconclusive` respectively** — never propagated, never left to `std::terminate()`. This is a structural requirement on the implementation, not an optional hardening pass.

## 9. Concurrency / ownership model (fixes round-5 P1 — shared mutable state had no defined owner)

`RequestWeightTracker`, `OrderCountTrackerSet`, `SymbolRegistry` (§5.3), and `DurableAuditSink` are all shared, mutable state touched by every one of: order submission, reconciliation queries, account refresh, and symbol-registry refresh. None of the existing code (`RequestWeightTracker` in particular) is internally synchronized — no mutex, no atomics beyond what a single-threaded caller would need. Revision 4 never stated who is allowed to call into this state concurrently, which is a real gap: two of these paths racing (e.g. a reconciliation query's rate-limit-header correction interleaved with a submit's pre-flight `can_send()` check) would be a data race, not just a design nicety left unspecified.

**Fix — single-owner-thread model, not fine-grained locking.** All of L4/L5 gate-chain execution (submit, reconciliation, account refresh, registry refresh, durable control-plane writes) runs on **one dedicated, non-hot-path owner thread** (the L5 actor when L5 is present; the L4 actor alone for read-only deployments). That thread is the **only** caller of `DurableControlPlaneSink::append_*` / L5 `append_durable` — OS advisory file locks do **not** serialize same-process threads, so a second thread calling `append_snapshot` would race `sequence_number`/`prev_mac` (round-9 P0).

**Operator refresh — fixes round-9 P0:** an admin/CLI thread must **not** call `SymbolRegistry::refresh_from_exchange_info(DurableControlPlaneSink&)` directly. It enqueues a `RefreshRegistry` task onto the owner actor (§9.1); only the owner thread performs exchangeInfo fetch, `append_snapshot`, and the in-memory publish under `std::shared_mutex`. `current_rules()` remains callable under the shared mutex from other threads for read-only snapshot copies; durable writes never leave the owner thread. If a future revision needs true concurrent submission, that requires an explicit new design — not a second writer smuggled onto the sink.

### 9.1 The single owner thread must be an actor/scheduler, not a blocking loop — fixes round-6 P1

Establishing a single-owner thread solves the synchronization problem, but round 6 correctly flagged that it introduces a new one if that thread is implemented naively: an earlier revision of §6.2/§6.3's reconciliation loop called a function literally named `sleep_respecting_backoff_and_freeze(...)`, and if the L5 thread is a plain `while(true) { ...; sleep(...); }` loop, **that same thread is the only one that can service account refresh (L4 §4), clock-offset resync (L4 §2.2), or any future kill-switch/control/health check** — all of which this design already requires to happen promptly (a stale clock offset is a fail-closed condition for every signed request; a kill-switch is meant to act fast). A reconciliation backoff sleeping for tens of seconds would starve all of them for that entire duration.

**Fix**: the L5 owner thread is structured as an **actor with a scheduled work queue**, not a blocking loop with inline sleeps. §6.2's reconciliation loop reflects this directly — it calls `schedule_reconcile_retry_at(rec, deadline_ms)` (an absolute-deadline re-enqueue, fixes round-7 P0's `Retry-After` ABI mismatch too, §7.3), never a `sleep()`-shaped function:

- Every unit of work (a submit attempt, a reconciliation query, an account refresh, a clock resync, a control/kill-switch check) is a discrete task with either "run now" or "run at/after time T" semantics — backoff is implemented as **re-enqueuing the reconciliation task for a future time**, not as the thread blocking in place. The thread's main loop is: pop the next task whose scheduled time has arrived (or block only on an empty queue / until the next scheduled time, whichever is sooner), run it to completion, repeat.
- **Priority**: control-plane tasks (kill-switch checks, operator commands) are checked/serviced ahead of reconciliation/backoff tasks on every loop iteration, regardless of queue order — a kill-switch request must not wait behind a reconciliation order's multi-attempt backoff schedule.
- Clock-offset resync (L4 §2.2) and account refresh are scheduled as their own periodic tasks in this same queue, with their own proactive-refresh timing (already specified for clock offset: "attempted proactively before the TTL expires") — never blocked behind whatever the reconciliation loop happens to be waiting on.
- A single `BinancePrivateRestClient` network call (`submit_order()`/`query_order()`) is still a blocking call *within* the task that issues it (per-phase deadlines, §2 of the L5 spec, bound its maximum duration) — "actor, not blocking loop" describes the thread's scheduling structure between tasks, not a requirement that every individual network I/O become asynchronous. A single slow network call still only blocks for its own bounded deadline, not indefinitely.

### 9.2 Cross-thread publish/subscribe semantics — fixes round-6 P1 (undefined for `current_rules_version()` and the clock-offset refresh)

§9's single-owner model established that durable writes never leave the owner actor; `SymbolRegistry::current_rules()` remains the one read path that may cross threads under `std::shared_mutex`. Two more values cross a thread boundary and were left undefined:

- **`SubmitPort::CurrentRulesVersionFn`** (§2.1 of the L5 spec, Gate 1's fast pre-check) ultimately reads the same `SymbolRegistry` state — it is implemented as a call into `SymbolRegistry::current_rules(symbol_id).rules_version` (or an equivalent single-field accessor under the same `std::shared_mutex`), **not** a separately-cached integer that could itself drift out of sync with the registry it's supposed to summarize. One source of truth, one lock, reused for both the full-snapshot read (§5.3's `current_rules()`) and the fast version-only read (Gate 1).
- **Clock-offset resync** (L4 §2.2) is, like registry refresh, a background/init-path operation the spec already says happens "proactively... on a background/init path, not inline with an order-critical request" — meaning it too can legitimately run on a thread other than the single L5 owner thread (e.g. a dedicated timer/background-refresh thread), and the offset it publishes must be readable by the L5 owner thread without a torn read. **Fix (revised round 12)**: publish via `ClockOffsetPublisher` (§7.1.2) — `std::atomic<std::shared_ptr<const ClockOffsetSnapshot>>` (or a mutex-guarded copy). Seqlock over non-atomic fields is **forbidden** (C++ data race / UB). Readers hold the `shared_ptr` for the duration of use. `SymbolRules` remains under `std::shared_mutex`.

## 10. Durable control-plane store — fixes round-9 P0 (L4↔L5 cycle)

Round 8 placed `RateLimitFreezePayload` / `append_rate_freeze` / (effectively) `append_snapshot` in L5, while L4 account refresh, startup baseline, and `SymbolRegistry::refresh_from_exchange_info` all require those writes — creating a reverse dependency that contradicts "L5 depends on accepted L4." This section is the shared foundation.

**Single shared header — fixes round-10 P0 (ODR / ABI drift):** every type in this section is defined **exactly once**, in `native/include/hengyuan/durable_control_plane.hpp` (new). L5's `DurableAuditSink` header `#include`s it; the L5 spec may *quote* these definitions for readability but a second C++ definition anywhere is a defect (same-name enums in two headers redefine in any TU that sees both, and the copies can silently drift). The spec-text blocks below **are** that header's contents, not a "mirror" of it.

```cpp
// hengyuan/durable_control_plane.hpp — the ONLY definition site.
//
// CHANGED — fixes this round's P0: multiple call sites need the sequence
// number a successful append was actually assigned (§6.2's
// `OrderOpenPollFailuresReset::reset_after_seq` must equal the polled
// event's own frame sequence; §10.3's `GenerationBridgePayload::prev_tip_seq`
// is the predecessor's actual last-assigned sequence; recovery-time
// cross-referencing throughout §6.1.x needs it too) — but a bare
// Acked/Failed enum gives the caller no way to learn what sequence its own
// just-written frame received. `AuditAppendResult` becomes a small struct;
// `.status` replaces every existing bare-enum comparison throughout both
// spec documents (`!= AuditAppendResult::Acked` becomes
// `.status != AuditAppendResult::Status::Acked`, or the equivalent
// `!result.acked()` using the convenience method below) — this is the one
// central definition change every `append_*`/`append_durable`/
// `append_order_checkpoint` call site in both files is written against;
// `.sequence` is the new capability this round adds.
struct AuditAppendResult {
    enum class Status : std::uint8_t { Acked = 0, Failed = 1 };
    Status status{Status::Failed};
    std::uint64_t sequence{0};  // the frame-sequence number actually assigned
                                  // to this write; valid ONLY if status == Acked.
                                  // Monotonic within a generation (§10.2);
                                  // callers needing to reference "the sequence
                                  // I just wrote" (e.g. a poll-reset event's
                                  // reset_after_seq, or a bridge frame's
                                  // prev_tip_seq) read this field directly
                                  // rather than re-deriving or guessing it.
    bool acked() const noexcept { return status == Status::Acked; }
};
enum class RecoveryScanStatus : std::uint8_t {
    Clean = 0,
    Recovered = 1,
    Corrupt = 2,
    CapacityExceeded = 3,
    IoError = 4,
    // Round-11 P0: previously named only in prose ("out-of-band"). Must be a
    // real enumerator so recovery can return it and the startup path can
    // require an explicit override (§10.2) rather than inventing a side channel.
    ExternalAnchorUnavailable = 5,
};

// Round-39 P0 — MUST NOT reuse RecoveryScanStatus for seal-by-request queries:
// that enum has no Ok/Found and conflates transport unavailability with
// "record absent." Abandon is legal only on authenticated NotFound (below).
enum class SealQueryStatus : std::uint8_t {
    Found = 0,                 // remote durably holds this request_id; out filled
    NotFound = 1,              // authenticated negative: remote asserts this
                               // request_id will never become durable (signed /
                               // quorum-negative). NOT a transport miss.
    TransportUnavailable = 2,  // cannot confirm presence (lag, not indexed,
                               // timeout, unreachable) — keep SealExportStarted
    Corrupt = 3,               // MAC fail / equivocation / bind mismatch
};

enum class DurableRecordType : std::uint8_t {
    // Round-11 P0: OrderEvent MUST be a real enumerator (value 0).
    OrderEvent = 0,
    SymbolRegistrySnapshot = 1,
    RateLimitFreeze = 2,
    TransportFailover = 3,       // §1.1 allowlist failover audit
    EndpointWeightConfigSet = 4, // §7.4 versioned weight table
    RateLimitUsageSnapshot = 5,  // §7.5.1 clean-shutdown usage baseline
    OperatorOverride = 6,        // round-12 P0 — external-anchor admission
    GenerationBridge = 7,        // round-12 P0 — compaction predecessor seal
    OrderCheckpoint = 8,         // revision-13 self-contained replay establish
    FreezeProbeAttempt = 9,      // NEW — fixes this round's P0: §7.3.1's
                                   // FreezeProbeCredit claimed a durable,
                                   // recoverable attempt count with no
                                   // backing record type; this is it.
    FreezeEpochWatermark = 10,   // NEW — fixes this round's P0: freeze_epoch's
                                   // durable, monotonic, never-reset high-water
                                   // mark (below) — survives compaction dropping
                                   // old RateLimitFreeze/FreezeProbeAttempt
                                   // records once their episode resolves.
    FreezeClear = 11,            // round-22 P0 — typed terminal clear
                                   // (ProbeVerified / ConservativeWaitCompleted /
                                   // OperatorAuthorized); never overload probe
                                   // attempts to clear a permanent fence.
    FreezeWaitSatisfied = 12,    // round-25/26 P0 — NON-terminal durable evidence
                                   // that conservative_wait_ms elapsed for an
                                   // epoch (sink-verified vs prior FreezeWaitArm);
                                   // required before dual-deadline ProbeVerified
                                   // and before ConservativeWaitCompleted when
                                   // wait>0; does NOT clear the freeze.
    FreezeWaitArm = 13,          // round-26 P0 — NON-terminal arm of a
                                   // conservative wait; sink records
                                   // arm_ack_steady at Ack; WaitSatisfied is
                                   // illegal without a live-session Arm whose
                                   // elapsed steady meets the bound.
    RateLimitFreezeSnapshot = 14, // round-33 P0 — compaction-only FOLDED
                                   // aggregate for an uncleared epoch; NOT a
                                   // live event contribution; must not go
                                   // through append_rate_freeze.
    CompactedFreezeWaitEvidence = 15, // round-33/34 — compaction-only SINGLE
                                   // frame carrying CompactedFreezeWaitEvidencePayload
                                   // (embedded Arm+Satisfy + source bind); FORBIDDEN
                                   // to substitute dual FreezeWaitArm/Satisfy frames.
    SealJournalApplied = 16,       // round-39…46 — SINGLE-frame seal-journal
                                   // apply on-disk record (see SealJournalAppliedView
                                   // call ABI). Index key = {candidate_id,
                                   // journal_seq}; entry_mac MUST be
                                   // HY-SEALJRN-v1-recomputed (not opaque-only).
                                   // FORBIDDEN to split into business append +
                                   // later marker; FORBIDDEN payload-less ABI.
};

// CANONICAL in this header (fixes round-20 P0 — was only named in L5 prose,
// but L4 append_* must select it). L5 includes; never redefines.
enum class FrameTimeKind : std::uint8_t {
    ServerCorrectedUtc = 0, // recorded_utc_ms = local_utc + published offset
    UnknownBootstrap   = 1, // recorded_utc_ms MUST be 0
};

enum class FreezeClearKind : std::uint8_t {
    ProbeVerified = 0,             // /time past UTC deadline; not for source=2
    ConservativeWaitCompleted = 1, // unknown-time wait done; not for source=2
    OperatorAuthorized = 2,        // ONLY legal clear for source=2
};

struct RateLimitFreezePayload {
    // KIND semantics (§7.3) — independent of frame-header provenance.
    // Round-23/31: BOTH deadline_utc_ms and conservative_wait_ms may be
    // non-zero across an episode after merges (UTC copy-forward + wait
    // contributions). Recovery / live owner fold max(deadline), max(wait),
    // max(wait_generation) across ALL frames of the epoch — never latest
    // frame alone. Per-frame conservative_wait_ms is THIS EVENT's wait
    // contribution (0 if none), not a restated running max.
    // Permanent source=2: timed fields ignored for latching.
    // Never wall-clock invent deadline_utc_ms (§7.0).
    std::int64_t recorded_utc_ms{0};  // legacy payload mirror; prefer header
    std::int64_t deadline_utc_ms{0};  // absolute UTC; 0 = absent (not "now")
    std::int64_t conservative_wait_ms{0}; // THIS frame's wait contribution
    std::uint8_t source{0};           // 0=429 timed, 1=418 timed, 2=permanent,
                                      // 3=unknown-time-429 (missing Retry-After)
    std::uint8_t pad[3]{};
    std::uint32_t freeze_epoch{0};
    // Round-31 P0 — wait-evidence generation. Advanced by +1 iff THIS frame's
    // conservative_wait_ms contribution > 0 (incoming event wait; not a
    // restated max), even when folded max duration is unchanged. 0 contribution
    // → copy-forward prior folded gen. Folded episode gen = max across frames.
    // Arm/WaitSatisfied/probe/clear bind this; stale gen evidence is rejected.
    // Encode as trailing LE u32; legacy shorter payloads decode as 0
    // (unspecified — NOT generation 1; see §10 legacy-wait migration).
    std::uint32_t wait_generation{0};
};
// FOUND AND FIXED THIS ROUND (a real bug in the immediately-preceding
// round's own edit): the struct above was missing its closing `};` —
// a literal compile error that had gone unnoticed until this round's
// cross-reference pass. This is exactly the "fix A, silently break B"
// pattern flagged this round; fixed here, and taken as a reminder to
// re-read a struct's full extent after editing it, not just the lines
// directly touched.

// Attempt tracking ONLY (round-22): terminal clears use FreezeClear.
// Round-26 P0: Ack of this record is the linearization point that consumes
// probe budget — the /time send is illegal until Acked.
// Round-28/29: EVERY freeze-period /time uses this type — no TimeResyncCredit
// while frozen. ClockRepublishOrVerify may clear on the same response when
// deadline is past (round-29 P0 — withdraws "republish never clears").
enum class FreezeProbePurpose : std::uint8_t {
    DeadlineOrVerify       = 0,  // now_utc known; probing toward/past not_before
    ClockRepublishOrVerify = 1,  // now_utc absent at reserve; burns same 8-cap;
                                   // MAY ProbeVerified-clear if serverTime >=
                                   // deadline (+ WaitSatisfied when required)
};

struct FreezeProbeAttemptPayload {
    std::uint32_t freeze_epoch{0};
    std::uint32_t attempt_ordinal{0}; // 1-based within this freeze_epoch;
                                        // sink requires == durable_max + 1
    bool cleared{false};              // MUST be false on append; sink rejects
                                        // true. Legacy true does not clear
                                        // source=2; see FreezeClear.
    FreezeProbePurpose purpose{FreezeProbePurpose::DeadlineOrVerify};
    // Round-26/27 — durable not-before for the NEXT FreezeProbeCredit send.
    // MUST equal max(checked now_utc+backoff, folded_deadline_utc_ms) when
    // computed with a published clock; when now_utc absent but folded
    // deadline > 0, MUST equal that deadline (arms UTC gate). 0 only when
    // both backoff schedule and deadline are absent. Recovery takes
    // max(attempts.not_before, folded freeze deadline). Sink rejects a
    // payload whose not_before is strictly below the folded deadline when
    // deadline > 0 (round-27 P0 — no early schedule).
    // ClockRepublishOrVerify: not_before still carries folded deadline.
    std::int64_t not_before_utc_ms{0};
};

// Round-23 P0 — MAC-covered /time proof required for ProbeVerified clears.
// CHANGED — fixes this round's P0 + P1: clock_snapshot_seq/clock_offset_ms
// are now CONSTRUCTIBLE (ClockOffsetSnapshot::seq, above) and their
// verification scope is made explicit (was previously ambiguous — the
// review correctly noted no interface existed to look up a HISTORICAL
// snapshot by seq, and no such interface is being added: it isn't needed).
// Both fields are captured INLINE, synchronously, on the L5/L4 owner thread,
// at the moment the proof is built — immediately after receiving the /time
// response, via `auto snap = clock_offset_publisher.load(); proof.clock_snapshot_seq
// = snap.seq; proof.clock_offset_ms = snap.offset_ms;`. The sink's
// verification (below) is therefore INTERNAL CONSISTENCY only — it recomputes
// `server_time_ms` against `clock_offset_ms` and the request's own local
// timestamp to sanity-check the proof is self-coherent — never a lookup
// against some retained history of past snapshots (no such registry exists
// or needs to; correctness comes from binding the offset used to interpret
// the response to the response itself, not from re-deriving it independently
// after the fact). `clock_snapshot_seq` is carried for audit/diagnostic
// purposes (which offset-generation produced this reading) and is NOT
// independently re-verified against a live registry.
//
// P1 fix — trust boundary and replay binding made explicit: this proof is a
// LOCAL, tamper-evident attestation, not third-party proof of Binance's
// identity — Binance does not sign /time responses. Its actual guarantee is
// "this record cannot be forged or altered after being durably written
// without also controlling the tip/KEK key" (the same guarantee every other
// durable frame in this design has) — it does NOT independently prove the
// response was genuinely received from Binance rather than fabricated by a
// compromised process that already holds that key; that trust boundary is
// identical to every other signed/HMAC'd artifact in this system and isn't
// a gap unique to this proof. To prevent this specific record from being
// replayed across a DIFFERENT freeze episode or endpoint than the one it was
// actually generated for, the MAC domain is extended to bind the full
// request/response context, not just the response body:
// Canonical MAC input: `"HY-FREEZE-TIME-PROOF-v1" || freeze_epoch ||
// bound_deadline_utc_ms || request_nonce || server_time_ms ||
// clock_snapshot_seq || clock_offset_ms || tls_verified_host` — `freeze_epoch`
// and `bound_deadline_utc_ms` bind the proof to the EXACT episode it clears
// (a proof generated for one epoch cannot be replayed against another, even
// if the raw serverTime happened to satisfy both deadlines); `request_nonce`
// (a fresh random value generated per probe attempt, not reused) prevents
// replaying the SAME server response's proof twice; `tls_verified_host`
// records that this response arrived over a connection whose certificate was
// verified against the bound environment's host (L4 §8), tying the proof to
// the transport-level identity check that already happened rather than
// treating that check as separately, silently assumed.
// Round-26 P1: tls_verified_host is a REAL FIELD on FreezeTimeProbeProof
// (NUL-terminated, max 63 chars + NUL). Sink rebuilds the MAC from the
// durable payload alone — no out-of-band host parameter on append_freeze_clear.
// Must be non-empty and a member of the process EnvironmentBinding allowlist
// (the SNI/peer host the /time connection actually verified — `base_host()`
// or an allowlisted failover). Empty / non-allowlisted → reject.
struct FreezeTimeProbeProof {
    std::int64_t server_time_ms{0};       // Binance serverTime from /time
    std::int64_t bound_deadline_utc_ms{0}; // MUST equal folded active
                                            // deadline_utc_ms; clear iff
                                            // server_time_ms >= this
    std::uint32_t clock_snapshot_seq{0};  // audit-only — see comment above;
                                            // not independently re-verified
    std::int64_t clock_offset_ms{0};      // used for the sink's internal
                                            // consistency recomputation
    std::uint64_t request_nonce{0};       // NEW — fixes this round's P1:
                                            // fresh per probe attempt, binds
                                            // the proof to this ONE attempt,
                                            // never reused across attempts
    static constexpr std::size_t kTlsVerifiedHostMax = 64; // includes NUL
    char tls_verified_host[kTlsVerifiedHostMax]{}; // Round-26 P1 — required
    std::uint8_t time_response_mac[32]{}; // covers the extended canonical
                                            // input above, not just the
                                            // response body
};

// Round-22/23 — sole terminal-clear record.
struct FreezeClearPayload {
    std::uint32_t freeze_epoch{0};
    FreezeClearKind clear_kind{FreezeClearKind::ProbeVerified};
    std::uint8_t pad[3]{};
    // ProbeVerified — required; zeros illegal:
    FreezeTimeProbeProof time_proof{};
    // ConservativeWaitCompleted — required bind; zeros illegal for that kind.
    // For ProbeVerified / ConservativeWaitCompleted with wait>0: this field is
    // informational only; sink requires a prior sink-verified FreezeWaitSatisfied
    // for the current wait_generation (round-25/26/31), not this equality alone.
    std::int64_t bound_conservative_wait_ms{0};
    // Round-31 — when wait>0, MUST equal folded wait_generation; sink rejects
    // mismatch. When wait==0, MUST be 0.
    std::uint32_t bound_wait_generation{0};
    // OperatorAuthorized fields (ignored / zero for other kinds):
    std::uint64_t store_uuid_lo{0};
    std::uint64_t store_uuid_hi{0};
    std::uint32_t bound_generation{0};
    std::uint64_t bound_tip_seq{0};
    std::uint8_t bound_tip_mac[32]{};
    std::int64_t wall_utc_ms{0};
    std::int64_t expires_utc_ms{0};
    std::uint64_t nonce{0};
    std::uint32_t kek_key_id{0};
    char operator_id[32]{};
    std::uint8_t mac[32]{};
};

// Round-26/31 P0 — NON-terminal ARM of a conservative wait for an epoch.
// Written BEFORE waiting. Sink records arm_ack_steady (process-local) at Ack.
// Does not lift can_send*. Abandoned across process restart if no matching
// WaitSatisfied was Acked (live sink has no arm_ack_steady for a prior Arm).
struct FreezeWaitArmPayload {
    std::uint32_t freeze_epoch{0};
    std::int64_t bound_conservative_wait_ms{0}; // == folded wait at arm time
    std::uint32_t wait_generation{0};           // MUST == folded wait_generation
    std::uint32_t arm_ordinal{1};               // 1-based within this generation
};

// Round-25/26/31 P0 — NON-terminal wait-completion evidence for an epoch.
// Does not lift can_send*. Dual-deadline ProbeVerified and
// ConservativeWaitCompleted (wait>0) must find a matching Acked record for
// the CURRENT wait_generation. Sink MUST independently verify elapsed steady
// since the cited Arm's Ack in THIS process session — duration equality alone
// is never sufficient (equal-wait rematch must re-Arm under a new generation).
struct FreezeWaitSatisfiedPayload {
    std::uint32_t freeze_epoch{0};
    std::int64_t bound_conservative_wait_ms{0}; // == folded wait at write time
    std::uint32_t wait_generation{0};           // MUST == Arm and folded gen
    std::uint32_t satisfaction_ordinal{1};      // 1-based within this generation
    std::uint32_t arm_ordinal{0};               // MUST match an Acked Arm
    std::uint64_t arm_frame_seq{0};             // AuditAppendResult::sequence of Arm
    std::int64_t elapsed_steady_ms_claimed{0};  // MUST be >= bound; sink checks
                                                  // its own elapsed independently
};

// Round-34/36 P1 — THE sole on-disk shape for DurableRecordType::CompactedFreezeWaitEvidence.
// Exactly one frame; implementations MUST NOT emit separate FreezeWaitArm +
// FreezeWaitSatisfied as a compaction substitute (recovery/content-root would
// diverge). Field-by-field little-endian encode (L5 §6.1.1.1). Full-frame MAC
// covers format_version || record_type || sequence || time_kind ||
// recorded_utc_ms || length || payload_bytes (same domain as every other frame),
// including source_baseline_key_id (round-36). Does NOT create arm_ack_steady;
// does NOT re-check live elapsed.
struct CompactedFreezeWaitEvidencePayload {
    std::uint32_t freeze_epoch{0};
    std::uint32_t wait_generation{0};           // explicit; MUST be > 0
    std::int64_t bound_conservative_wait_ms{0}; // == folded wait
    // Embedded Arm (rewritten / retained values — not a second record type):
    std::uint32_t arm_ordinal{1};
    std::uint64_t source_arm_seq{0};            // Arm's sequence in source gen N
    // Embedded Satisfy:
    std::uint32_t satisfaction_ordinal{1};
    std::uint64_t source_satisfy_seq{0};        // Satisfy’s sequence in source gen N
    std::uint64_t satisfy_arm_frame_seq{0};     // MUST == source_arm_seq
    std::int64_t elapsed_steady_ms_claimed{0};  // copied; not re-validated live
    // Source-generation bind (must match CompactionSourceBaseline / proof):
    std::uint32_t source_generation{0};
    std::uint64_t source_baseline_tip_seq{0};
    std::uint8_t source_baseline_tip_mac[32]{};
    std::uint32_t source_baseline_key_id{0};    // round-36 — key that signs
                                                  // source_baseline_tip_mac;
                                                  // MUST == CompactionSourceBaseline.key_id
                                                  // == GenerationBridge.prev_key_id
    std::uint8_t legacy_sequence_proven_rewrite{0}; // 1 if path-3 rewrite
};

// NEW — fixes this round's P0: freeze_epoch's durable high-water mark.
// Stores the NEXT allocatable epoch (`next_freeze_epoch`). Using epoch E is
// legal only after Ack of a watermark with next_freeze_epoch == E+1
// (round-21 P0 — do NOT say "before the epoch it names"; the field names the
// next unused value, not the epoch being consumed). Never derived by scanning
// RateLimitFreeze/FreezeProbeAttempt (compaction may drop resolved episodes).
// Round-22: watermark advances ONLY when starting a new episode (no active
// uncleared epoch). Merges re-use the active epoch and do not touch watermark.
struct FreezeEpochWatermarkPayload {
    std::uint32_t next_freeze_epoch{1};  // next allocatable epoch; starts at 1
                                           // (0 reserved "never used");
                                           // monotonic; never decreases/resets
                                           // across restart or compaction.
};
```

**Assignment protocol (round-21/22: watermark only for NEW episodes; merge skips watermark):**

```cpp
// If an active (uncleared) episode already exists — MERGE, do not allocate:
if (out_has_freeze || out_permanent_latch) {
    // append_rate_freeze(merged_payload_same_epoch, time_kind); no watermark
    return;
}

// Else — genuinely NEW episode:
std::uint32_t recovered_next_epoch = /* recover_control_plane()'s
    out_next_freeze_epoch, or 1 if out_has_freeze_epoch_watermark was false */;

// CHANGED — fixes this round's P1: recovered_next_epoch + 1 is an
// unchecked std::uint32_t addition. At recovered_next_epoch == UINT32_MAX
// this silently wraps to 0 — reusing epoch 0, which is reserved as "never
// used" (§10) and would make a genuinely new freeze indistinguishable from
// "no watermark has ever been recorded," violating the "monotonic, never
// reused" invariant this whole mechanism exists to provide. At roughly one
// new freeze episode per second this takes over 136 years to reach, but an
// unchecked wraparound is exactly the class of bug this design refuses to
// leave latent elsewhere (§5.1.1's pow10_i64 bound, §7.3's Retry-After
// unrepresentable-deadline handling) — the same discipline applies here.
if (recovered_next_epoch == std::numeric_limits<std::uint32_t>::max()) {
    fence_and_alarm("freeze_epoch space exhausted — operator migration required");
    return;
}
// If out_uncleared_epoch_count > 1: refuse NEW allocation until fold/heal
// (merges into an existing uncleared epoch still allowed for fail-closed
// extension of waits / escalation to permanent).
if (out_uncleared_epoch_count > 1) {
    fence_and_alarm("multiple uncleared freeze epochs — compaction fold required");
    // still may merge into permanent's epoch or max-wait epoch per §7.3.1
}
const FrameTimeKind episode_time_kind =
    clock_offset_published() ? FrameTimeKind::ServerCorrectedUtc
                             : FrameTimeKind::UnknownBootstrap;
// Persist next=E+1 BEFORE using E.
if (!control_plane.append_freeze_epoch_watermark(
        recovered_next_epoch + 1, episode_time_kind).acked()) {
    return;
}
std::uint32_t this_freezes_epoch = recovered_next_epoch;
// ... append_rate_freeze(..., episode_time_kind)
// Round-26/27: only AFTER both watermark and freeze Acked:
//   credit.bind_new_episode_after_durable_create(
//       this_freezes_epoch, payload.deadline_utc_ms);
```

**Compaction retains the single latest `FreezeEpochWatermark` always.** For freezes (round-22/25/26/31/32/33/34): retain **every uncleared epoch**, folding **all** that epoch's live `RateLimitFreeze` frames (and any prior `RateLimitFreezeSnapshot`) into one aggregate (`max(deadline_utc_ms)`, `max(conservative_wait_ms)`, `max(wait_generation)`, worst `source`). Emit that aggregate **only** via `append_compacted_freeze_snapshot` into `gen-N+1` (never via live `append_rate_freeze`), and only while `CompactionSourceBaseline` tip is unchanged (§10.1). The snapshot carries **folded** field values and **must not** apply the live `prior_folded + 1` rule — including when folded `wait_generation > 1`. Also retain current-gen wait evidence **only if** `wait_generation > 0` matches folded gen — emit as **exactly one** `CompactedFreezeWaitEvidence` frame via `append_compacted_wait_evidence(CompactedFreezeWaitEvidencePayload)` (never live `append_freeze_wait_*`, never a dual Arm+Satisfy frame substitute). Drop stale-generation **and** all legacy gen `0`/missing Arm/Satisfy. Never drop uncleared `source=2` because a newer timed exists. Valid terminal `FreezeClear` + resolved probe history may be dropped; watermark retained.

**Payload field additive decode (round-31/32, header format stays v3):** `wait_generation` / `bound_wait_generation` encode as trailing little-endian `uint32` on their respective payloads (field-by-field, length-prefixed — L5 §6.1.1.1). A legacy shorter payload missing the trailing field decodes as `0` (**unspecified**, not “generation one”).

**Legacy-wait upgrade (round-32/33/34 P0 — replaces the withdrawn round-31 “synthesize gen=1 + match legacy Satisfy once” rule):**
- Missing/0 is **not** evidence identity. Recovery must **not** treat legacy Arm/Satisfy as `out_has_wait_satisfied` for any synthesized generation.
- Uncleared episode with folded wait `> 0` and `max(freeze.wait_generation) == 0` is **legacy-wait** (§7.3.1): require live migration and/or compaction seal to durable `wait_generation ≥ 1` before probe/clear; default is re-Arm → full wait → Satisfy under the sealed gen.
- Compaction of a legacy-wait epoch: `append_compacted_freeze_snapshot` with explicit `wait_generation = 1`; drop legacy Arm/Satisfy **unless** applying the sequence-proven rewrite (§7.3.1 path 3) in the **same** compaction transaction — requiring **both** Arm.seq and Satisfy.seq strictly after every wait-bearing live freeze, plus Satisfy citing that Arm; emit via `append_compacted_wait_evidence`. Rematch / Arm-before-last-wait-bearing → drop Satisfy → must re-wait after seal.
- Post-seal, further **live** wait-bearing merges use normal `append_rate_freeze` with `prior_folded + 1` (gen `2`, `3`, …). Tip drift after yield aborts the whole candidate (§10.1).

**Recovery fold note (round-33/34):** `RateLimitFreezeSnapshot` and live `RateLimitFreeze` both contribute to max-fold. A snapshot’s `conservative_wait_ms` / `wait_generation` are already folded values for that write; live frames remain per-event contributions. After a successful compaction that becomes CURRENT, the uncleared epoch typically has a single snapshot (+ optional single-frame `CompactedFreezeWaitEvidence`). Recovery projects that evidence into the logical Arm+Satisfy view for `out_has_wait_satisfied` (explicit gen `> 0` only).
```cpp

struct SymbolRegistrySnapshotPayload {
    std::int64_t timestamp_ms{0};
    std::uint32_t rules_version{0};
    std::uint32_t symbol_count{0};  // <= kMaxSymbols
    // followed by symbol_count SymbolRules entries in the same frame
};

// §7.5.1 — per-tracker/per-interval usage at clean shutdown. Fixed capacity,
// no heap: at most kMaxUsageEntries (weight intervals + raw intervals) rows.
struct RateLimitUsageSnapshotPayload {
    std::int64_t recorded_utc_ms{0};
    std::uint32_t entry_count{0};   // <= kMaxUsageEntries (= 8)
    struct Entry {
        std::uint8_t tracker{0};    // 0=REQUEST_WEIGHT, 1=RAW_REQUESTS
        char interval_suffix[7]{};  // "1M", "5M", ...
        std::int64_t bucket_start_server_ms{0};
        std::uint32_t used{0};
    } entries[8]{};
};

// Round-12 P0 — evidence that an operator authorized startup despite
// ExternalAnchorUnavailable. Written to the *sidecar* path (§10.2), not
// into a store that recovery has already refused.
struct OperatorOverridePayload {
    std::uint64_t store_uuid_lo{0};
    std::uint64_t store_uuid_hi{0};
    std::uint64_t local_tip_seq{0};
    std::uint8_t local_tip_mac[32]{}; // binds the exact tip bytes, not seq alone
    std::uint32_t local_generation{0};
    // Round-19 P0 — bind the durable export baseline this override authorizes
    // against. A new override MUST copy the on-disk LastRemoteAckedTip (or
    // zeros if never exported); it must NOT invent a "fresh empty backlog."
    std::uint32_t last_remote_acked_generation{0};
    std::uint64_t last_remote_acked_seq{0};
    std::uint8_t last_remote_acked_mac[32]{};
    std::uint8_t admit_mode{0};       // 0=AppendAllowedIfUnderHardLag,
                                        // 1=ReadOnlyDrain (forced when
                                        // inherited backlog already exceeds
                                        // hard-lag at issue time)
    std::int64_t wall_utc_ms{0};
    std::int64_t expires_utc_ms{0};  // short-lived, operator-selected
    std::uint64_t nonce{0};          // one-shot replay protection
    std::uint32_t kek_key_id{0};
    std::uint32_t reason_code{0};     // enumerated: ExternalAnchorDown=1, …
    char operator_id[32]{};           // fixed, not heap
    std::uint8_t mac[32]{};           // HMAC under KEK over the above fields
};

// Round-12 P0 — compaction predecessor seal bridging generation N → N+1.
// CHANGED — fixes this round's P0: `bridge_mac` was described as "under the
// tip key" with no field saying WHICH key — the predecessor generation's
// last-used key_id and the new generation's key_id are not guaranteed to be
// the same (this codebase's own §6.1.1.2 HMAC key rotation, from an earlier
// round, established that keys rotate). If a rotation happened exactly at
// the compaction boundary, a verifier with no way to know which key(s) to
// try could either false-flag a legitimate bridge as Corrupt (tried the
// wrong key) or, worse, be tempted to try multiple keys until one happens
// to verify — which weakens the MAC into "signed by ANY key we know about,"
// not "signed by the specific key claimed." Both `prev_key_id` and
// `new_key_id` are now explicit, MAC'd fields, and the MAC domain is pinned.
struct GenerationBridgePayload {
    std::uint32_t prev_generation{0};
    std::uint64_t prev_tip_seq{0};
    std::uint8_t prev_tip_mac[32]{};
    std::uint32_t prev_key_id{0};       // NEW — the key_id prev_tip_mac was
                                          // actually computed under; verifying
                                          // prev_tip_mac uses THIS key, never
                                          // "whichever key is current now"
    std::uint32_t new_generation{0};
    std::uint64_t new_genesis_seq{0};  // usually 0
    std::uint32_t new_key_id{0};        // NEW — the key_id this bridge frame
                                          // ITSELF (and the new generation's
                                          // subsequent frames) is written under
    std::uint8_t bridge_mac[32]{};     // NEW MAC DOMAIN — HMAC(new_key_id,
                                          // "HY-GENBRIDGE-v1" || prev_generation
                                          // || prev_tip_seq || prev_tip_mac ||
                                          // prev_key_id || new_generation ||
                                          // new_genesis_seq), canonical
                                          // declaration-order little-endian
                                          // encoding matching §10.2's
                                          // GenerationSeal convention. Always
                                          // keyed by new_key_id (the bridge
                                          // frame lives IN the new generation,
                                          // §10.3's write order) — prev_key_id
                                          // is carried as authenticated DATA
                                          // (covered by bridge_mac) so a
                                          // verifier who already trusts
                                          // new_key_id can look up prev_key_id
                                          // from the verified payload itself,
                                          // rather than needing to already
                                          // know it out-of-band.
};

struct GenerationSeal {
    std::uint64_t store_uuid_lo{0};
    std::uint64_t store_uuid_hi{0};
    GenerationBridgePayload bridge{};
    std::uint64_t final_seq{0};
    std::uint8_t final_tip_mac[32]{};
    std::uint32_t key_id{0};
    std::uint8_t content_root[32]{}; // SHA-256 root — see §10.2 point 4 content-root rule
    // Round-37/39 — idempotency key. MUST equal SealExportStarted.request_id.
    // Allocated only via SealIdWatermark (§10.1); never a free-running RNG /
    // wall-clock / reused counter across restart.
    std::uint64_t request_id{0};
    // Round-38 — normative MAC domain (little-endian field order, no padding):
    // HMAC(key_id, "HY-GENSEAL-v1" || store_uuid_lo || store_uuid_hi ||
    //   bridge_mac || final_seq || final_tip_mac || key_id ||
    //   content_root || request_id)
    // where bridge_mac is GenerationBridgePayload::bridge_mac (already covers
    // the prev/new bridge fields). key_id in the domain is this seal's key_id.
    std::uint8_t mac[32]{};
};

// Round-39/41/42 P0 — durable high-water for seal ids (breadcrumb, outside store).
// next_* is the NEXT allocatable value (like FreezeEpochWatermark).
// Cross-file atomicity with SealExportStarted is IMPOSSIBLE on ordinary FS —
// "same barrier" prose is WITHDRAWN (round-41).
// Round-42 — reservation MUST precede any Path B journal (entries need
// candidate_id). Normative PreSeal order:
//   1. After tip-drain + quiesce: assign candidate_id + request_id from watermark;
//      advance next_* += 1; durable-replace+flush SealIdWatermark;
//   2. In-flight drain may Path B under that durable candidate_id;
//   3. Later CREATE_NEW+flush SealExportStarted reusing THOSE ids (no second
//      watermark advance); then first seal network write.
// Crash after (1) before (3): ids consumed forever; journaled entries →
// PreSeal replay-to-N only (never invent Started / never reclaim ids).
// Crash after (3): PostSeal; recovery MUST verify watermark already past
// Started's ids. Never decrease; never reuse. At UINT64_MAX → fence.
struct SealIdWatermark {
    std::uint64_t store_uuid_lo{0};
    std::uint64_t store_uuid_hi{0};
    std::uint64_t next_candidate_id{1};
    std::uint64_t next_request_id{1};
    // HMAC(KEK, "HY-SEALIDWM-v1" || store_uuid_lo || store_uuid_hi ||
    //   next_candidate_id || next_request_id)
    std::uint8_t mac[32]{};
};

// Round-47/48 P0 — per-candidate journal commit high-water (breadcrumb directory).
// File: seal-journal/<store_uuid…>/<candidate_id_hex16>.jhw
// Advanced ONLY after final `.sj1` for that seq is durable; monotonic — never
// decreases (round-48: durable replace MUST load-verify; regression → Corrupt).
// Sole authoritative source for next journal_seq (= highest + 1).
// Lifetime: CREATE_NEW at durable id-reserve (highest=0) and MUST remain
// until linearizable intake-closed + GC phase A (round-51/52). NEVER delete
// while Path B may still run. Residual `.jts` deleted in phase B after `.jhw`
// gone. Drain-complete = no `.sj1` / no `.jhw` / no `.jts` — see §10.1.
struct SealJournalCommitWatermark {
    std::uint64_t store_uuid_lo{0};
    std::uint64_t store_uuid_hi{0};
    std::uint64_t candidate_id{0};
    std::uint64_t highest_committed_journal_seq{0};  // 0 = none yet
    std::uint32_t kek_key_id{0};
    // HMAC(KEK[kek_key_id], "HY-SEALJRNHW-v1" || store_uuid_lo ||
    //   store_uuid_hi || candidate_id || highest_committed_journal_seq ||
    //   kek_key_id)
    std::uint8_t mac[32]{};
};

// Round-52/53 — linearizable PostSeal handoff intake-close (RAM control block;
// NOT a durable breadcrumb — after process restart, close_epoch resets to 0 and
// the owner MUST re-run the full protocol before GC A; never trust a prior
// process's quiesce). One instance per active candidate_id. Fixed-size,
// NO heap; atomics cacheline-aligned (CLAUDE.md false-sharing rule).
//
// kMaxSealHandoffProducers is the ARRAY CAPACITY (≤8), NOT the wait set.
// The wait set is registered_producer_mask (round-53 P0) — true-SPSC ⇒
// mask=0b1, count=1; per-producer array ⇒ exactly the bits for live writers.
// mask is uint8_t ⇒ capacity MUST stay ≤8 (round-54 P1).
constexpr std::size_t kMaxSealHandoffProducers = 8;
static_assert(kMaxSealHandoffProducers <= 8,
              "registered_producer_mask is uint8_t; raise mask width before kMax");
// Default close budget (owner may tighten); process-kill / hung producer must
// not busy-spin forever (round-53 P1).
constexpr std::uint64_t kSealJournalIntakeCloseDeadlineMs = 5'000;
constexpr std::uint32_t kSealJournalIntakeCloseMaxPollIters = 1'000'000;
using SealHandoffRingId = std::uint32_t;  // compile-time / config ring identity
struct alignas(std::hardware_destructive_interference_size)
    SealJournalIntakeCloseProducerSlot {
    // Written by producer i with release after it has: observed close_epoch==E,
    // refused new try_push for E, and finished any admit that already incremented
    // in_flight_admit_guard. 0 = not quiesced for current epoch.
    std::atomic<std::uint64_t> quiesced_ack_epoch{0};
};
struct SealJournalIntakeCloseControl {
    // Frozen BEFORE SealExportStarted / PostSeal handoff start (single-writer
    // owner). Immutable until drain-complete. popcount(mask)==producer_count;
    // bit i set ⇒ producers[i] + ring_id[i] are in the close wait set.
    // Round-54: same tuple is MAC-bound into SealExportStarted (durable).
    std::uint64_t candidate_id{0};             // MUST match this candidate
    std::uint8_t registered_producer_mask{0};  // bits 0..kMax-1 only
    std::uint8_t producer_count{0};            // 1..kMax; 0 illegal after freeze
    SealHandoffRingId ring_id[kMaxSealHandoffProducers]{};  // per-slot ring bind
    bool topology_frozen{false};               // true after freeze; false→no close

    // Owner stores non-zero close_epoch E with release to begin close.
    // Producers load with acquire; if close_epoch != 0 they must not start a
    // new try_push. 0 = intake open for Path B handoff admits.
    // After timeout: epoch stays armed; same-process retry MUST store E+1
    // (prior quiesced_ack_epoch==old E is void — round-54 P1).
    alignas(std::hardware_destructive_interference_size)
        std::atomic<std::uint64_t> close_epoch{0};
    // Producer increments (acq_rel) BEFORE claiming a ring slot / publishing
    // tail; decrements after successful publish OR abandoned push.
    // ONLY producers whose bit is set in registered_producer_mask may touch
    // this guard; unset-bit fetch_add / try_push → immediate hard fence.
    // Owner may treat rings as finally empty only when this is 0 (acquire).
    alignas(std::hardware_destructive_interference_size)
        std::atomic<std::uint32_t> in_flight_admit_guard{0};
    SealJournalIntakeCloseProducerSlot producers[kMaxSealHandoffProducers]{};

    // Owner-local (single-writer), set when close begins:
    // std::chrono::steady_clock::time_point close_deadline_steady;
    // (conceptual — implement with steady_clock, not wall clock.)
    // Owner-local: set true only after close protocol SUCCESS.
    bool path_b_prohibited{false};
};

// Round-48 P1 — per-seq clear receipt after Applied Ack (breadcrumb directory).
// File: seal-journal/<store_uuid…>/<candidate_id_hex16>-<journal_seq_hex16>.jts
// Packed LE, NO padding. CREATE_NEW / no-replace (same class as .sj1 finals).
// MUST include entry_mac so a silently replaced .sj1 cannot be "cleared" under
// a mismatched payload identity.
// Layout:
//   +0   u32 format_version (=1)
//   +4   u32 total_bytes    (= 108)
//   +8   u64 store_uuid_lo
//   +16  u64 store_uuid_hi
//   +24  u32 kek_key_id
//   +28  u64 candidate_id
//   +36  u64 journal_seq
//   +44  u8  entry_mac[32]   // MUST equal the Applied / journal entry_mac
//   +76  u8  mac[32]         // trailer
constexpr std::uint32_t kSealJournalTombstoneFormatVersion = 1;
constexpr std::size_t kSealJournalTombstoneBytes = 108;
struct SealJournalTombstoneWire {
    // Conceptual; on-disk is packed as above. MAC domain (LE, no padding):
    // HMAC(KEK[kek_key_id], "HY-SEALJRNTS-v1" || format_version || total_bytes ||
    //   store_uuid_lo || store_uuid_hi || kek_key_id || candidate_id ||
    //   journal_seq || entry_mac)
    std::uint8_t mac[32]{};
};

// Round-37/54/55 — durable breadcrumb OUTSIDE the store root (same class as
// LastRemoteAckedTip / OperatorOverrideSidecar). Written+fsynced BEFORE any
// seal network byte is sent. Presence forces PostSeal recovery even when no
// local seal Ack was observed.
//
// Round-55/57 — on-disk is packed LE wire (NOT a compiler-layout struct).
// Conceptual field names below match SealExportStartedWire.
// Layout (packed LE, NO padding) — format_version == 2:
//   +0    u32 format_version (=2)
//   +4    u32 total_bytes    (= 238 == kSealExportStartedWireBytes)
//   +8    u64 store_uuid_lo
//   +16   u64 store_uuid_hi
//   +24   u64 candidate_id
//   +32   u32 source_generation
//   +36   u64 baseline_tip_seq
//   +44   u8  baseline_tip_mac[32]
//   +76   u32 baseline_key_id
//   +80   u32 new_generation
//   +84   u64 new_final_seq
//   +92   u8  new_final_tip_mac[32]
//   +124  u32 new_key_id
//   +128  u64 request_id
//   +136  u8  content_root[32]
//   +168  u32 kek_key_id          // round-57 P0 — selects KEK[kek_key_id]
//   +172  u8  registered_producer_mask
//   +173  u8  producer_count
//   +174  u32 ring_id[8]          // kMaxSealHandoffProducers; unset → 0
//   +206  u8  mac[32]             // trailer
// Filenames (breadcrumb dir) — round-56/57/64:
//   seal-export-started       — greenfield v2 OR legacy 192B final (immutable)
//   seal-export-started.v2    — migration companion (CREATE_NEW / no-replace)
//   seal-export-started.mig   — migration commit (CREATE_NEW / no-replace)
//   seal-export-started.clr   — cleanup tombstone (CREATE_NEW; phase via REPLACE)
//   seal-export-started.abd   — ClrAbandoned proof (CREATE_NEW; phase via REPLACE)
//   compaction-candidate-intent — CompactionCandidateIntentWire (CREATE_NEW
//     Building; phase via REPLACE Building→Reserved→StartedPublished→
//     PostSealFinalizing | AbandonFinalizing — REPLACE only AFTER durable
//     matching CompactionIntentTransitionWire `.x1`; r66)
//   compaction-intent-x-<build_nonce_hex16>-<seq_hex16>.x1 —
//     CompactionIntentTransitionWire (CREATE_NEW / no-replace; NEVER
//     REPLACE; same class as `.sj1` hardlink publish; r66)
//   compaction-intent-gc-<build_nonce_hex16>.xgc —
//     CompactionIntentGcAuthorizedWire (CREATE_NEW / no-replace; NEVER
//     REPLACE; GC authorization receipt; r67)
// Forbidden for L/V/M / `.x1` / `.xgc`: §10.3 REPLACE / replace-MoveFileExW /
// delete-then recreate of a live final (power-cut → lose PostSeal gate
// or erase the only crash-verifiable Intent edge / GC auth).
// `.clr` / `.abd` / Intent phase advances MAY use §10.3 REPLACE
// (watermark class) — monotonic raise only, and Intent REPLACE only
// after matching durable `.x1` (r66).
constexpr std::uint32_t kSealExportStartedFormatVersion = 2;
constexpr std::size_t kSealExportStartedWireBytes = 238;
// Legacy pre-topology conceptual length (r37…r53, no format_version header):
// 192 = fields through content_root + mac under "HY-SEALSTART-v1" (no kek_key_id).
constexpr std::size_t kSealExportStartedLegacyV1Bytes = 192;
// Draft-only 234B (r55/r56 before kek_key_id) — never admit; treat as Corrupt.
constexpr std::size_t kSealExportStartedDraft234Bytes = 234;
static_assert(kSealExportStartedWireBytes == 238);
static_assert(4 + 4 + 8 + 8 + 8 + 4 + 8 + 32 + 4 + 4 + 8 + 32 + 4 + 8 + 32 + 4 + 1 + 1
                  + (4 * kMaxSealHandoffProducers) + 32
              == kSealExportStartedWireBytes);
struct SealExportStartedWire {
    // Conceptual; on-disk packed as above. MAC domain (LE, no padding):
    // HMAC(KEK[kek_key_id], "HY-SEALSTART-v2" || format_version || total_bytes ||
    //   store_uuid_lo || store_uuid_hi || candidate_id || source_generation ||
    //   baseline_tip_seq || baseline_tip_mac || baseline_key_id ||
    //   new_generation || new_final_seq || new_final_tip_mac || new_key_id ||
    //   request_id || content_root || kek_key_id || registered_producer_mask ||
    //   producer_count || ring_id[0] || … || ring_id[kMax-1])
    // Topology MUST match frozen SealJournalIntakeCloseControl (round-54/55).
    // Forbidden: write format_version!=2; Forbidden: total_bytes!=238;
    // Forbidden: invent mask=0 / empty topology; Forbidden: try-all / current-key
    // fallback when kek_key_id wrapper missing (L5 §6.1.1.2).
    std::uint8_t mac[32]{};
};
// Alias used in prose: SealExportStarted == SealExportStartedWire v2 fields.
using SealExportStarted = SealExportStartedWire;

// Round-56/57 — migration commit (companion strategy). Packed LE, NO padding.
// File: seal-export-started.mig
// Layout (format_version == 2):
//   +0   u32 format_version (=2)
//   +4   u32 total_bytes    (= 208)
//   +8   u64 store_uuid_lo
//   +16  u64 store_uuid_hi
//   +24  u64 candidate_id
//   +32  u64 request_id
//   +40  u32 legacy_kek_key_id  // sole KEK used to verify L (explicit; no try-all)
//   +44  u32 v2_kek_key_id      // == V.kek_key_id; selects KEK for V + M MACs
//   +48  u8  legacy_file_digest[32]  // SHA-256(entire L file bytes)
//   +80  u8  v2_file_digest[32]      // SHA-256(entire V file bytes)
//   +112 u8  legacy_mac[32]          // L trailer MAC
//   +144 u8  v2_mac[32]              // V trailer MAC
//   +176 u8  mac[32]
constexpr std::uint32_t kSealExportStartedMigrationFormatVersion = 2;
constexpr std::size_t kSealExportStartedMigrationWireBytes = 208;
// Draft-only r56 mig (macs+ids, no kek ids / no full-file digests) — Corrupt.
constexpr std::size_t kSealExportStartedMigrationDraft136Bytes = 136;
static_assert(4 + 4 + 8 + 8 + 8 + 8 + 4 + 4 + 32 + 32 + 32 + 32 + 32
              == kSealExportStartedMigrationWireBytes);
struct SealExportStartedMigrationWire {
    // HMAC(KEK[v2_kek_key_id], "HY-SEALSTARTMIG-v2" || format_version ||
    //   total_bytes || store_uuid_lo || store_uuid_hi || candidate_id ||
    //   request_id || legacy_kek_key_id || v2_kek_key_id ||
    //   legacy_file_digest || v2_file_digest || legacy_mac || v2_mac)
    std::uint8_t mac[32]{};
};

// Round-57/58 P0 — cleanup tombstone (authorizes Started unlink ONLY after
// PostSeal committed: CURRENT flipped + tip persisted + bridge bound +
// journal drain-complete). File: seal-export-started.clr
// Layout (format_version == 2) — packed LE, NO padding:
//   +0    u32 format_version (=2)
//   +4    u32 total_bytes    (= 304 == kSealStartedCleanupWireBytes)
//   +8    u64 store_uuid_lo
//   +16   u64 store_uuid_hi
//   +24   u64 candidate_id
//   +32   u64 request_id
//   +40   u32 kek_key_id
//   +44   u8  started_kind     // 1=NativeV2, 2=MigratedV2
//   +45   u8  present_mask     // bit0=L, bit1=V, bit2=M at authorize time
//   +46   u8  phase            // 0=Authorized … see cleanup order below
//   +47   u8  reserved0 (=0)
//   // PostSealCommittedProof (round-58 P0) — MAC-bound; all MUST hold at
//   // CREATE and at every CleanupInProgress resume:
//   +48   u32 source_generation      // == Started.source_generation == bridge.prev_generation
//   +52   u64 baseline_tip_seq       // == Started.baseline_tip_seq == bridge.prev_tip_seq
//   +60   u8  baseline_tip_mac[32]   // == Started / bridge.prev_tip_mac
//   +92   u32 baseline_key_id        // == Started / bridge.prev_key_id
//   +96   u32 new_generation         // == Started.new_generation; CURRENT MUST == this
//   +100  u64 new_final_seq          // == Started.new_final_seq
//   +108  u8  new_final_tip_mac[32]  // == Started.new_final_tip_mac
//   +140  u32 new_key_id             // == Started.new_key_id
//   +144  u8  content_root[32]       // == Started.content_root == GenerationSeal.content_root
//   +176  u8  digest_L[32]           // SHA-256(L) or zeros if bit0 clear
//   +208  u8  digest_V[32]
//   +240  u8  digest_M[32]
//   +272  u8  mac[32]
constexpr std::uint32_t kSealStartedCleanupFormatVersion = 2;
constexpr std::size_t kSealStartedCleanupWireBytes = 304;
// Draft-only r57 .clr (ids/digests only; no PostSealCommittedProof) — Corrupt /
// never CleanupInProgress (never pad to 304).
constexpr std::size_t kSealStartedCleanupDraft176Bytes = 176;
constexpr std::uint8_t kSealStartedKindNativeV2 = 1;
constexpr std::uint8_t kSealStartedKindMigratedV2 = 2;
constexpr std::uint8_t kSealStartedCleanupPhaseAuthorized = 0;
constexpr std::uint8_t kSealStartedCleanupPhaseMGone = 1;
constexpr std::uint8_t kSealStartedCleanupPhaseVGone = 2;
constexpr std::uint8_t kSealStartedCleanupPhaseLGone = 3;
constexpr std::uint8_t kSealStartedCleanupPhaseClrPending = 4;  // only CLR left
static_assert(4 + 4 + 8 + 8 + 8 + 8 + 4 + 1 + 1 + 1 + 1
                  + 4 + 8 + 32 + 4 + 4 + 8 + 32 + 4 + 32
                  + 32 + 32 + 32 + 32
              == kSealStartedCleanupWireBytes);
struct SealStartedCleanupTombstoneWire {
    // HMAC(KEK[kek_key_id], "HY-SEALSTARTCLR-v2" || format_version ||
    //   total_bytes || store_uuid_lo || store_uuid_hi || candidate_id ||
    //   request_id || kek_key_id || started_kind || present_mask || phase ||
    //   reserved0 || source_generation || baseline_tip_seq ||
    //   baseline_tip_mac || baseline_key_id || new_generation ||
    //   new_final_seq || new_final_tip_mac || new_key_id || content_root ||
    //   digest_L || digest_V || digest_M)
    // phase advances are monotonic; REPLACE of .clr allowed only to raise phase
    // (proof fields immutable after Authorized CREATE_NEW).
    std::uint8_t mac[32]{};
};

// Round-59 P1 — ClrAbandoned proof (converges ClrUnauthorized + NotFound).
// File: seal-export-started.abd
// Layout (format_version == 1) — packed LE, NO padding:
//   +0    u32 format_version (=1)
//   +4    u32 total_bytes    (= 192 == kSealStartedAbandonWireBytes)
//   +8    u64 store_uuid_lo
//   +16   u64 store_uuid_hi
//   +24   u64 candidate_id
//   +32   u64 request_id
//   +40   u32 kek_key_id
//   +44   u8  started_kind     // 1=NativeV2, 2=MigratedV2
//   +45   u8  abandon_reason   // 1=AuthenticatedNotFound
//   +46   u8  present_mask     // bit0=L, bit1=V, bit2=M, bit3=C at authorize
//   +47   u8  phase            // see ClrAbandoned order below
//   +48   u32 source_generation
//   +52   u64 baseline_tip_seq
//   +60   u8  baseline_tip_mac[32]
//   +92   u32 baseline_key_id
//   +96   u8  content_root[32]
//   +128  u8  digest_C[32]     // SHA-256(unauthorized C) or zeros if absent/torn
//   +160  u8  mac[32]
constexpr std::uint32_t kSealStartedAbandonFormatVersion = 1;
constexpr std::size_t kSealStartedAbandonWireBytes = 192;
constexpr std::uint8_t kSealStartedAbandonReasonNotFound = 1;
constexpr std::uint8_t kSealStartedAbandonPhaseAuthorized = 0;
constexpr std::uint8_t kSealStartedAbandonPhaseCGone = 1;
constexpr std::uint8_t kSealStartedAbandonPhaseMGone = 2;
constexpr std::uint8_t kSealStartedAbandonPhaseVGone = 3;
constexpr std::uint8_t kSealStartedAbandonPhaseLGone = 4;
constexpr std::uint8_t kSealStartedAbandonPhaseGenGone = 5;  // gen-N+1 abandoned
constexpr std::uint8_t kSealStartedAbandonPhaseResumeAuthorized = 6;  // tip ok; A kept; producer paused
constexpr std::uint8_t kSealStartedAbandonPhaseAbdPending = 7;  // unlink A next; resume after A gone
static_assert(4 + 4 + 8 + 8 + 8 + 8 + 4 + 1 + 1 + 1 + 1
                  + 4 + 8 + 32 + 4 + 32 + 32 + 32
              == kSealStartedAbandonWireBytes);
struct SealStartedAbandonWire {
    // HMAC(KEK[kek_key_id], "HY-SEALSTARTABD-v1" || format_version ||
    //   total_bytes || store_uuid_lo || store_uuid_hi || candidate_id ||
    //   request_id || kek_key_id || started_kind || abandon_reason ||
    //   present_mask || phase || source_generation || baseline_tip_seq ||
    //   baseline_tip_mac || baseline_key_id || content_root || digest_C)
    // phase advances monotonic; REPLACE .abd only to raise phase
    // (all other fields immutable after Authorized CREATE_NEW).
    std::uint8_t mac[32]{};
};

// Round-64/65/66/67 — CompactionCandidateIntent (distinguishes legal PreSeal-build
// remnant from Started-cleared-without-A0; r65 adds terminal Finalizing phases
// so nearly-finished PostSeal/abandon cleanup is not false-Corrupt; r66 adds
// no-replace CompactionIntentTransitionWire so no-cross/no-skip is crash-
// verifiable — Intent.phase REPLACE alone is insufficient; r67 adds
// CompactionIntentGcAuthorizedWire `.xgc` so authorized mid-GC `.x1` gaps
// are not false-Corrupt).
// File: compaction-candidate-intent (breadcrumb dir; same durability class
// as SealExportStarted).
// Layout (format_version == 1) — packed LE, NO padding; **wire size stays 140B**
// (r65 extends phase enum only — no layout growth; no silent size break):
//   +0    u32 format_version (=1)
//   +4    u32 total_bytes    (= 140 == kCompactionCandidateIntentWireBytes)
//   +8    u64 store_uuid_lo
//   +16   u64 store_uuid_hi
//   +24   u32 kek_key_id
//   +28   u8  phase            // 0=Building … 4=AbandonFinalizing (see below)
//   +29   u8  reserved0 (=0)   // MUST be 0; no completion_kind (live phase
//                                discriminator; path proof = `.x1` chain r66)
//   +30   u16 reserved1 (=0)
//   +32   u32 source_generation
//   +36   u32 target_generation  // == source_generation + 1 (UINT32_MAX refuse)
//   +40   u64 baseline_tip_seq
//   +48   u8  baseline_tip_mac[32]
//   +80   u32 baseline_key_id
//   +84   u64 build_nonce        // unique per build attempt; never reused
//   +92   u64 candidate_id       // 0 while Building; bound at Reserved
//   +100  u64 request_id         // 0 while Building; bound at Reserved
//   +108  u8  mac[32]
constexpr std::uint32_t kCompactionCandidateIntentFormatVersion = 1;
constexpr std::size_t kCompactionCandidateIntentWireBytes = 140;
constexpr std::uint8_t kCompactionCandidateIntentPhaseBuilding = 0;
constexpr std::uint8_t kCompactionCandidateIntentPhaseReserved = 1;
constexpr std::uint8_t kCompactionCandidateIntentPhaseStartedPublished = 2;
constexpr std::uint8_t kCompactionCandidateIntentPhasePostSealFinalizing = 3;  // r65
constexpr std::uint8_t kCompactionCandidateIntentPhaseAbandonFinalizing = 4;   // r65
static_assert(4 + 4 + 8 + 8 + 4 + 1 + 1 + 2 + 4 + 4 + 8 + 32 + 4 + 8 + 8 + 8 + 32
              == kCompactionCandidateIntentWireBytes);
struct CompactionCandidateIntentWire {
    // HMAC(KEK[kek_key_id], "HY-COMPINTENT-v1" || format_version ||
    //   total_bytes || store_uuid_lo || store_uuid_hi || kek_key_id ||
    //   phase || reserved0 || reserved1 || source_generation ||
    //   target_generation || baseline_tip_seq || baseline_tip_mac ||
    //   baseline_key_id || build_nonce || candidate_id || request_id)
    // CREATE_NEW / no-replace for phase=Building (before any gen-N+1 write).
    // Genesis = this CREATE_NEW (no none→Building transition receipt).
    // REPLACE allowed ONLY to raise phase along ONE legal chain, and ONLY
    // AFTER a durable matching CompactionIntentTransitionWire `.x1` for
    // that edge exists (r66 — Intent.phase alone is NOT crash-verifiable
    // path proof; monotonic REPLACE does NOT prove no-cross after reboot):
    //   Building→Reserved→StartedPublished→PostSealFinalizing
    //   OR Building→Reserved→StartedPublished→AbandonFinalizing
    // Forbidden: PostSealFinalizing↔AbandonFinalizing cross; Forbidden: jump
    // Building|Reserved→*Finalizing; Forbidden: any phase regression;
    // Forbidden: Intent REPLACE without prior durable matching `.x1`.
    // Proof fields (baseline 4-tuple, generations, build_nonce, store_uuid,
    // kek_key_id) immutable after CREATE_NEW. candidate_id/request_id may
    // change from 0→nonzero exactly once at Reserved (must match
    // SealIdWatermark reservation); nonzero→other nonzero → Corrupt.
    // reserved0/reserved1 MUST stay 0 — **no** parallel completion_kind field
    // (phase is the live terminal/non-terminal discriminator; crash-verifiable
    // path / no-cross proof is CompactionIntentTransitionWire `.x1`, r66).
    // Hot-path: preallocated fixed 140B buffer; no heap; no std::string.
    // Flush: same class as SealExportStarted (§10.3) — file + parent.
    // Windows: complete MAC-valid Intent ⇒ durable Intent present
    // (parent-dir flush undecidable; do not invent "not durable" PreSeal
    // on a complete file — same honesty rule as Started).
    // Clear (CAPTURE cleanup evidence → unlink C/A → CREATE `.xgc` →
    // TipExportProducerResume idempotent → GC matching `.x1` → unlink Intent →
    // unlink `.xgc` last; parent flush after each create/unlink) ONLY after
    // this candidate's final cleanup complete (r67/r70/r71 — `.xgc` authorizes
    // partial `.x1` absence only when CREATE-time + Mode-B
    // PhysicalCleanupPreconditions hold; Mode B reads DurableCleanupAuthEvidence
    // from `.xgc`, never deleted `.clr`/`.abd`):
    //   (a) PreSeal-abandon (Building|Reserved): after G/T/journal/Started/A/C
    //       cleanup as applicable (GenGone rules for pre-Started) → CREATE
    //       `.xgc` (PreSealAbandonClear; evidence zeros; Building-only
    //       terminal_transition_mac=0) → TipExportProducerResume if pause
    //       armed (idempotent) → GC `.x1` → Intent → `.xgc`;
    //   (b) PostSeal success: after Intent PostSealFinalizing + ordered unlink
    //       M→V→L → CAPTURE from C → unlink C → CREATE `.xgc`
    //       (PostSealFinalizingClear; DurableCleanupAuthEvidence bound) →
    //       TipExportProducerResume (idempotent) → GC `.x1` → Intent → `.xgc`;
    //   (c) ClrAbandoned: after Intent AbandonFinalizing + GenGone +
    //       ResumeAuthorized → CAPTURE from A → unlink A → CREATE `.xgc`
    //       (AbandonFinalizingClear; DurableCleanupAuthEvidence bound) →
    //       TipExportProducerResume (idempotent) → GC `.x1` → Intent → `.xgc` —
    //       Intent clear AFTER A unlink, never before GenGone; never leave
    //       Intent forever blocking new build.
    // Forbidden: CREATE Intent while prior Intent still present (dual /
    // leftover Intent incl. *Finalizing / leftover `.xgc` → finish prior
    // cleanup first; never overwrite);
    // Forbidden: CREATE `.xgc` before the matching (a)/(b)/(c) authorization
    // moment / while Started|A|C (or PostSeal L/V/M) still gates cleanup /
    // wrong disposition / without DurableCleanupAuthEvidence capturable;
    // Forbidden: write G before durable Building Intent;
    // Forbidden: raise Reserved without durable SealIdWatermark advance
    // binding the same candidate_id/request_id;
    // Forbidden: raise StartedPublished before durable NativeV2/
    // MigratedV2 Started for those ids;
    // Forbidden: raise PostSealFinalizing before `.clr` Authorized +
    // PostSealCommittedProof;
    // Forbidden: raise AbandonFinalizing before ResumeAuthorized while A
    // present (or without GenGone done);
    // Forbidden: clear Intent while Started / `.abd` / live G for this
    // build still gates cleanup (except after *Finalizing proves the
    // terminal path and those names are already gone / finishable);
    // Forbidden: clear Intent without prior durable matching `.xgc`;
    // Forbidden: unlink any matching `.x1` without prior durable `.xgc`.
    std::uint8_t mac[32]{};
};


// Round-66 P1 — CompactionIntentTransitionWire (no-replace transition receipt).
// Intent.phase alone is REPLACE-overwriteable and CANNOT prove historical path
// (no-skip / no PostSeal↔Abandon cross) across reboot. Every phase RAISE must
// first publish a durable transition receipt; recovery verifies the full chain.
// Genesis: CREATE_NEW Intent phase=Building IS the genesis — there is NO
// none→Building transition file. Transition chain starts at seq=1:
//   Building→Reserved→StartedPublished→PostSealFinalizing
//   OR Building→Reserved→StartedPublished→AbandonFinalizing
// File (breadcrumb dir; same durability class as .sj1 finals — NEVER REPLACE):
//   compaction-intent-x-<build_nonce_hex16>-<transition_seq_hex16>.x1
// Publish recipe (align journal §10.1 / Windows): encode preallocated buffer →
//   exclusive-create `*.x1.tmp` → fsync(file) → no-replace publish
//   (POSIX renameat2(RENAME_NOREPLACE)/linkat; Windows preferred
//   CreateHardLinkW → parent FlushFileBuffers → unlink tmp; CREATE_NEW copy
//   fallback only) → parent dir flush. If final exists: MAC-verify + byte-equal
//   → idempotent; any difference → Corrupt, original unchanged.
// Forbidden: §10.3 REPLACE / replace-MoveFileExW / delete-then-recreate of a
// live `.x1` (power-cut would erase the only crash-verifiable edge).
// Layout (format_version == 1) — packed LE, NO padding; **fixed 176B**:
//   +0    u32 format_version (=1)
//   +4    u32 total_bytes    (= 176 == kCompactionIntentTransitionWireBytes)
//   +8    u64 store_uuid_lo
//   +16   u64 store_uuid_hi
//   +24   u32 kek_key_id
//   +28   u8  from_phase       // Intent phase BEFORE this raise
//   +29   u8  to_phase         // Intent phase AFTER this raise
//   +30   u16 reserved0 (=0)
//   +32   u32 transition_seq   // 1-based; first raise Building→Reserved = 1
//   +36   u32 source_generation
//   +40   u32 target_generation
//   +44   u64 baseline_tip_seq
//   +52   u8  baseline_tip_mac[32]
//   +84   u32 baseline_key_id
//   +88   u64 build_nonce      // MUST equal Intent.build_nonce
//   +96   u64 candidate_id     // post-raise bind (0 only illegal for seq>=1)
//   +104  u64 request_id       // post-raise bind (nonzero from seq=1 onward)
//   +112  u8  prev_transition_mac[32]  // all-zero iff transition_seq==1;
//                                      // else == trailer mac of seq-1 `.x1`
//   +144  u8  mac[32]
constexpr std::uint32_t kCompactionIntentTransitionFormatVersion = 1;
constexpr std::size_t kCompactionIntentTransitionWireBytes = 176;
static_assert(4 + 4 + 8 + 8 + 4 + 1 + 1 + 2 + 4 + 4 + 4 + 8 + 32 + 4
                  + 8 + 8 + 8 + 32 + 32
              == kCompactionIntentTransitionWireBytes);
struct CompactionIntentTransitionWire {
    // HMAC(KEK[kek_key_id], "HY-COMPINTENT-X-v1" || format_version ||
    //   total_bytes || store_uuid_lo || store_uuid_hi || kek_key_id ||
    //   from_phase || to_phase || reserved0 || transition_seq ||
    //   source_generation || target_generation || baseline_tip_seq ||
    //   baseline_tip_mac || baseline_key_id || build_nonce ||
    //   candidate_id || request_id || prev_transition_mac)
    // Legal edges ONLY (from_phase→to_phase):
    //   0→1 Building→Reserved                 (seq must be 1)
    //   1→2 Reserved→StartedPublished         (seq must be 2)
    //   2→3 StartedPublished→PostSealFinalizing (seq must be 3)
    //   2→4 StartedPublished→AbandonFinalizing  (seq must be 3)
    // Forbidden edges (always Corrupt if present): 3→4, 4→3, any skip
    // (0→2/0→3/0→4/1→3/1→4/…), any regression, duplicate seq, seq gap.
    // Bind MUST match Intent: store_uuid, kek_key_id, baseline 4-tuple,
    // generations, build_nonce. candidate_id/request_id MUST equal the
    // post-Reserved Intent ids (nonzero from seq=1). Transition for a
    // foreign build_nonce / wrong baseline while Intent exists → Corrupt.
    // Hot-path: preallocated fixed 176B buffer; no heap; no std::string.
    // Control-plane I/O only.
    std::uint8_t mac[32]{};
};

// Round-67/70/71 P0 — CompactionIntentGcAuthorizedWire (no-replace GC authorization).
// r66 path-proof `.x1` chain + "gap ⇒ Corrupt" + "GC all `.x1` before Intent"
// created a false-Corrupt power window: legal mid-GC (e.g. deleted x1[1] from
// {1,2,3}, Intent still present) looks like an unauthorized seq gap. Fixed by
// a durable GC-authorization receipt that must exist BEFORE any `.x1` unlink.
// r70 required Mode B to re-check physical cleanup preconditions, but those
// checks re-read `.clr`/`.abd` which normative order already unlinked — legal
// mid-GC crash made Mode B non-constructible. r71 embeds
// **DurableCleanupAuthEvidence** into `.xgc` itself (one coherent artifact;
// no TerminalCleanupProof sibling) and verifies Mode B from that evidence +
// live gate absence.
// File (breadcrumb dir; same durability class as `.x1` / `.sj1` — NEVER REPLACE):
//   compaction-intent-gc-<build_nonce_hex16>.xgc
// Publish recipe: encode preallocated 316B → exclusive-create `*.xgc.tmp` →
//   fsync(file) → no-replace publish (POSIX renameat2(RENAME_NOREPLACE)/linkat;
//   Windows preferred CreateHardLinkW → parent FlushFileBuffers → unlink tmp;
//   CREATE_NEW copy fallback only) → parent dir flush. If final exists:
//   MAC-verify + byte-equal → idempotent; any difference → Corrupt, original
//   unchanged. Forbidden: §10.3 REPLACE / replace-MoveFileExW / delete-then-
//   recreate of a live `.xgc`.
// Layout (format_version == 2) — packed LE, NO padding; **fixed 316B**:
//   +0    u32 format_version (=2)
//   +4    u32 total_bytes    (= 316 == kCompactionIntentGcAuthorizedWireBytes)
//   +8    u64 store_uuid_lo
//   +16   u64 store_uuid_hi
//   +24   u32 kek_key_id
//   +28   u8  terminal_disposition
//         // 0 = PreSealAbandonClear
//         // 1 = PostSealFinalizingClear
//         // 2 = AbandonFinalizingClear
//   +29   u8  intent_phase_at_auth  // MUST == Intent.phase at CREATE time
//   +30   u16 reserved0 (=0)
//   +32   u32 source_generation
//   +36   u32 target_generation
//   +40   u64 baseline_tip_seq
//   +48   u8  baseline_tip_mac[32]
//   +80   u32 baseline_key_id
//   +84   u64 build_nonce          // MUST equal Intent.build_nonce
//   +92   u64 candidate_id
//   +100  u64 request_id
//   +108  u8  intent_mac[32]       // Intent trailer mac at CREATE time
//   +140  u8  terminal_transition_mac[32]
//         // trailer mac of last (highest-seq) matching `.x1` at CREATE;
//         // all-zero IFF no `.x1` exist AND disposition==PreSealAbandonClear
//         // AND intent_phase_at_auth==Building (Building-only clear)
//   // DurableCleanupAuthEvidence (r71) — Mode B reconstructible w/o live C/A:
//   +172  u8  cleanup_auth_flags
//         // bit0 = journal_drain_complete_at_auth
//         // bit1 = post_seal_committed_bound   (PostSealFinalizingClear)
//         // bit2 = gen_gone_complete_at_auth   (AbandonFinalizingClear)
//         // bit3 = resume_authorized_bound     (AbandonFinalizingClear;
//         //         gate_trailer_mac is A.mac at phase≥ResumeAuthorized)
//         // bit4 = gate_absent_at_create       (1 only on recovery CREATE when
//         //         C/A already unlinked; live crash-free path MUST be 0)
//         // bit5..7 = 0
//   +173  u8  started_kind          // 0 PreSeal; else C/A.started_kind (1/2)
//   +174  u8  present_mask_at_auth  // 0 PreSeal; C mask (L/V/M) or A mask
//   +175  u8  reserved1 (=0)
//   +176  u64 proof_new_final_seq           // PostSeal: C.new_final_seq; else 0
//   +184  u8  proof_new_final_tip_mac[32]   // PostSeal: C.new_final_tip_mac
//   +216  u32 proof_new_key_id              // PostSeal: C.new_key_id; else 0
//   +220  u8  proof_content_root[32]        // PostSeal C / Abandon A; PreSeal 0
//   +252  u8  gate_trailer_mac[32]
//         // PostSeal: C.mac captured before unlink C (live path);
//         // Abandon: A.mac at phase≥ResumeAuthorized captured before unlink A;
//         // PreSeal: all-zero;
//         // gate_absent_at_create=1: MUST be all-zero (recovery reconstruction)
//   +284  u8  mac[32]
// Legacy v1 204B / domain HY-COMPINTENT-GC-v1 (r67…r70): **fail-closed** —
// never Mode B; never pad/truncate to 316; offline migration only.
constexpr std::uint32_t kCompactionIntentGcAuthorizedFormatVersion = 2;
constexpr std::size_t kCompactionIntentGcAuthorizedWireBytes = 316;
constexpr std::size_t kCompactionIntentGcAuthorizedLegacyV1Bytes = 204;
constexpr std::uint8_t kCompactionIntentGcDispositionPreSealAbandonClear = 0;
constexpr std::uint8_t kCompactionIntentGcDispositionPostSealFinalizingClear = 1;
constexpr std::uint8_t kCompactionIntentGcDispositionAbandonFinalizingClear = 2;
constexpr std::uint8_t kCompactionIntentGcAuthFlagJournalDrain = 1u << 0;
constexpr std::uint8_t kCompactionIntentGcAuthFlagPostSealBound = 1u << 1;
constexpr std::uint8_t kCompactionIntentGcAuthFlagGenGone = 1u << 2;
constexpr std::uint8_t kCompactionIntentGcAuthFlagResumeAuthorized = 1u << 3;
constexpr std::uint8_t kCompactionIntentGcAuthFlagGateAbsentAtCreate = 1u << 4;
static_assert(4 + 4 + 8 + 8 + 4 + 1 + 1 + 2 + 4 + 4 + 8 + 32 + 4
                  + 8 + 8 + 8 + 32 + 32
                  + 1 + 1 + 1 + 1 + 8 + 32 + 4 + 32 + 32 + 32
              == kCompactionIntentGcAuthorizedWireBytes);
struct CompactionIntentGcAuthorizedWire {
    // HMAC(KEK[kek_key_id], "HY-COMPINTENT-GC-v2" || format_version ||
    //   total_bytes || store_uuid_lo || store_uuid_hi || kek_key_id ||
    //   terminal_disposition || intent_phase_at_auth || reserved0 ||
    //   source_generation || target_generation || baseline_tip_seq ||
    //   baseline_tip_mac || baseline_key_id || build_nonce ||
    //   candidate_id || request_id || intent_mac || terminal_transition_mac ||
    //   cleanup_auth_flags || started_kind || present_mask_at_auth ||
    //   reserved1 || proof_new_final_seq || proof_new_final_tip_mac ||
    //   proof_new_key_id || proof_content_root || gate_trailer_mac)
    //
    // === CREATE-time PhysicalCleanupPreconditions (may read live C/A) ===
    // CREATE_NEW / no-replace ONLY when terminal cleanup already authorizes
    // Intent clear **AND** DurableCleanupAuthEvidence is captured into the
    // wire (r71 — phase alone is insufficient; TipExportProducerResume is
    // NOT a CREATE/Mode-B authorization fact):
    //   PreSealAbandonClear (0): Intent.phase ∈ {Building, Reserved};
    //     PreSeal cleanup complete for G/T/journal/Started/A/C as applicable
    //     (pre-Started GenGone rules); no admitted Started; A absent; C absent;
    //     cleanup_auth_flags bits1..4 == 0; started_kind/present_mask/
    //     proof_new_*/proof_content_root/gate_trailer_mac all zero;
    //     bit0 journal_drain as applicable (1 if candidate had journal work).
    //   PostSealFinalizingClear (1): Intent.phase == PostSealFinalizing;
    //     **L/V/M already cleared**; C was readable in the capture window
    //     (live path) OR gate_absent_at_create recovery reconstruction;
    //     PostSealCommittedProof held at capture (CURRENT==new_generation;
    //     tip equal-or-forward covers seal tip; bridge binds baseline +
    //     content_root; journal drain-complete); wire MUST set bit0+bit1;
    //     gate_trailer_mac = C.mac (live path, bit4=0) or 0 (bit4=1);
    //     proof_new_* / proof_content_root / started_kind / present_mask
    //     from C (or GenerationSeal/bridge/tip reconstruction when bit4=1).
    //     NOTE: Intent.phase / intent_phase_at_auth == PostSealFinalizing is
    //     **NOT** proof cleanup complete — that phase is raised **before**
    //     Started/C unlink.
    //   AbandonFinalizingClear (2): Intent.phase == AbandonFinalizing;
    //     GenGone + ResumeAuthorized done; Started/C already cleared; A was
    //     readable in the capture window (live path) OR gate_absent_at_create
    //     recovery reconstruction; wire MUST set bit0+bit2+bit3;
    //     gate_trailer_mac = A.mac at phase≥ResumeAuthorized (live, bit4=0)
    //     or 0 (bit4=1); proof_content_root = A.content_root (or T bind when
    //     bit4=1); proof_new_* = 0; started_kind/present_mask from A.
    //
    // === Crash-safe capture → unlink → CREATE order (r71; mandatory) ===
    //   PostSeal (after Intent PostSealFinalizing + ordered unlink M→V→L,
    //   C still present at ClrPending):
    //     1) CAPTURE into preallocated buffer (same actor critical section;
    //        **no yield** / no schedule point): C.mac, C.started_kind,
    //        C.present_mask, C.new_final_seq/mac/key_id, C.content_root,
    //        journal_drain_complete, PostSealCommittedProof live-hold.
    //     2) unlink C → parent flush.
    //     3) CREATE `.xgc` binding captured digests + Intent binds
    //        (bit4=0; gate_trailer_mac=captured C.mac).
    //     4) TipExportProducerResume (**idempotent**; not durable auth).
    //     5) GC matching `.x1` → clear Intent → unlink `.xgc` last.
    //   Abandon (after ResumeAuthorized + Intent AbandonFinalizing, A present):
    //     1) CAPTURE: A.mac (≥ResumeAuthorized), A.started_kind/present_mask/
    //        content_root, gen_gone + resume_authorized facts.
    //     2) unlink A → parent flush.
    //     3) CREATE `.xgc` (bit4=0; gate_trailer_mac=captured A.mac).
    //     4) TipExportProducerResume (idempotent).
    //     5) GC `.x1` → clear Intent → unlink `.xgc` last.
    //   Crash after C/A unlink, before CREATE `.xgc`: nearly-finished path
    //     (Intent *Finalizing, no `.xgc`) reconstructs evidence with bit4=1
    //     (no live gate_trailer_mac) then CREATEs `.xgc` — NOT Corrupt.
    //   Forbidden: yield between capture and CREATE on the live path;
    //   Forbidden: CREATE while L/V/M/Started still present (PostSeal) or
    //     while A/Started/C still present (Abandon) — capture-then-unlink
    //     first; Forbidden: CREATE while Intent absent; Forbidden: separate
    //     TerminalCleanupProof sibling (dual-artifact); Forbidden: Mode B
    //     that re-reads deleted `.clr`/`.abd`.
    //
    // Bind MUST match Intent at CREATE: store_uuid, kek_key_id, baseline
    // 4-tuple, generations, build_nonce, candidate_id/request_id,
    // intent_mac == Intent.mac, intent_phase_at_auth == Intent.phase,
    // disposition↔phase pairing above. terminal_transition_mac MUST equal
    // last `.x1` trailer mac when any `.x1` exist; zeros only for Building-
    // only PreSeal clear. Forbidden: CREATE while StartedPublished mid-path;
    // Forbidden: CREATE before DurableCleanupAuthEvidence is capturable /
    // reconstructible; Forbidden: dual `.xgc` (same or foreign build_nonce)
    // while prior GC incomplete; Forbidden: REPLACE / overwrite live `.xgc`;
    // Forbidden: wrong disposition vs Intent.phase; Forbidden: intent_mac /
    // transition mac / baseline / build_nonce / cleanup-evidence mismatch
    // (forged → Corrupt); Forbidden: format_version!=2 / total_bytes!=316 /
    // legacy v1 204B admitted as Mode B.
    // Hot-path: preallocated fixed 316B buffer; no heap; no std::string.
    // Control-plane I/O only.
    std::uint8_t mac[32]{};
};

// CompactionIntentTransition — normative raise / recovery / GC (round-66 P1
// + round-67/70/71 P0 `.xgc`):
//   Atomic-like raise (EVERY phase raise after genesis) — UNCHANGED:
//     1) Encode + no-replace publish matching `.x1` for next transition_seq
//        (from_phase = current Intent.phase; to_phase = next legal phase;
//        prev_transition_mac = prior `.x1` mac or zeros for seq=1) →
//        file+parent flush until durable.
//     2) ONLY THEN REPLACE Intent.phase → to_phase (recompute HY-COMPINTENT-v1
//        MAC; parent flush). Never Intent REPLACE without a prior durable
//        matching transition whose to_phase equals the new Intent.phase.
//   Genesis: CREATE_NEW Intent Building — no `.x1`. First raise (Reserved)
//     publishes seq=1 Building→Reserved before REPLACE.
//   Recovery chain verify — TWO modes:
//     **Mode A — no Mode-B-eligible `.xgc` for this build_nonce (mid-path /
//     unauthorized):**
//       (If a MAC-valid `.xgc` exists but Mode-B PhysicalCleanupPreconditions
//       fail → **Corrupt** early/forged `.xgc` immediately; do **not** treat
//       as Mode A mid-path and do **not** Mode B GC. Legacy v1 204B `.xgc`
//       → **Corrupt**/fail-closed — never Mode B.)
//       Load all `.x1` for Intent.build_nonce; sort by seq. Contiguous seq
//       1..N; MAC-ok under wire kek_key_id; filename==wire; prev_mac chain
//       unbroken; only legal edges; binds match Intent. Legal full chains:
//       (1,2,3) with 2→3 PostSealFinalizing OR (1,2,3) with 2→4
//       AbandonFinalizing. Partial prefixes OK while in-progress (1; 1+2;
//       1+2+3). Missing link / jump / cross / seq gap / prev_mac break /
//       foreign bind → **Corrupt**. Intent.phase vs last transition T*:
//         (a) no `.x1`:
//             - Intent.phase == Building → OK (genesis).
//             - Intent.phase > Building → **Corrupt** (missing path proof
//               OR unauthorized `.x1` GC without `.xgc` — r66 GC-lag finish-
//               clear WITHOUT authorization artifact is **withdrawn**).
//         (b) Intent.phase == T*.to_phase: converged OK (then classify).
//         (c) Intent.phase == T*.from_phase: **one-step lag** — complete
//             Intent REPLACE to T*.to_phase; NOT Corrupt; NOT accept cross.
//         (d) Intent.phase > T*.to_phase without matching further `.x1`:
//             **Corrupt**.
//         (e) Intent.phase < T*.from_phase (other than lag c): Corrupt.
//     **Mode B — MAC-valid verified `.xgc` (v2/316B/`HY-COMPINTENT-GC-v2`)
//     for this build_nonce AND Mode-B PhysicalCleanupPreconditions hold
//     (authorized GC; r70/r71 — “valid `.xgc` ⇒ Mode B” shorthand WITHDRAWN;
//     r71 — preconditions are durable-evidence + live gate absence, NOT
//     re-read of deleted `.clr`/`.abd`):**
//       **Step 1 — MAC/bind verify:** Verify `.xgc` under wire kek_key_id;
//       format_version==2 && total_bytes==316; filename build_nonce == wire;
//       binds == Intent (if Intent present) for store_uuid / kek / baseline /
//       generations / build_nonce / ids; intent_mac == Intent.mac;
//       intent_phase_at_auth == Intent.phase; disposition↔phase legal;
//       DurableCleanupAuthEvidence flags/fields consistent with disposition
//       (PostSeal: bit0+bit1; Abandon: bit0+bit2+bit3; PreSeal: bits1..4==0
//       and zeroed seal/gate fields; bit4 only with zero gate_trailer_mac);
//       if any `.x1` remain: each MAC-ok + bind==Intent/`.xgc`;
//       terminal_transition_mac must equal the highest-seq remaining `.x1`
//       mac OR (if that seq already unlinked) equal the mac recorded at
//       CREATE (do not require contiguous 1..N).
//       **Step 2 — BEFORE any Mode B converge GC, verify Mode-B
//       PhysicalCleanupPreconditions(terminal_disposition) FROM `.xgc`
//       DurableCleanupAuthEvidence + live absence of leftover gates**
//       (identical authorization *meaning* as CREATE-time, but MUST NOT
//       re-read deleted `.clr`/`.abd`; MUST NOT require TipExportProducerResume
//       "already executed" as a durable fact):
//         PreSealAbandonClear (0): evidence zeros/flags legal; PreSeal cleanup
//           complete for G/T/journal as applicable; no admitted Started;
//           A absent; C absent; if tip-export pause still armed →
//           TipExportProducerResume **idempotently** (not a fail precondition).
//         PostSealFinalizingClear (1): `.xgc` bit0+bit1 set; proof_new_* /
//           proof_content_root / started_kind legal; gate_trailer_mac nonzero
//           iff bit4=0; live **L/V/M/C/Started all absent**; live CURRENT ==
//           target_generation / proof_new_generation path; tip equal-or-forward
//           covers bound seal tip; bridge binds baseline + proof_content_root;
//           journal remnants for candidate absent; then TipExportProducerResume
//           idempotently if pause armed.
//         AbandonFinalizingClear (2): `.xgc` bit0+bit2+bit3 set;
//           proof_content_root legal; gate_trailer_mac nonzero iff bit4=0;
//           live **A/Started/C all absent**; CURRENT/tip consistent with
//           bound abandon baseline (equal-or-forward rules as A3 residue);
//           abandoned T bind matches proof_content_root when T present; then
//           TipExportProducerResume idempotently if pause armed.
//       Any `.xgc` coexisting with leftover gates above = **early `.xgc` →
//       Corrupt**; MUST NOT enter Mode B GC (forged or buggy early write of
//       MAC-legal `.xgc` while Intent=PostSealFinalizing but `.clr`/Started
//       still present, or Abandon A still present). Note:
//       intent_phase_at_auth=PostSealFinalizing is **not** proof cleanup
//       complete — that phase is raised **before** Started/C unlink.
//       Forged cleanup evidence (flags/fields disagree with disposition, or
//       bit4=1 with nonzero gate_trailer_mac, or PostSeal missing bit1, etc.)
//       → **Corrupt**.
//       **If Step 1+2 pass:** **Gaps / partial `.x1` set are NOT Corrupt**
//       under Mode B — recovery MUST **converge GC**:
//         TipExportProducerResume idempotently if needed → unlink remaining
//         matching `.x1` (any order OK; prefer ascending seq) → parent flush
//         each → unlink Intent (if present) → parent flush → unlink `.xgc`
//         last → parent flush.
//       Intent already gone + Step 1+2 pass (+ residual `.x1`): finish unlink
//       residual `.x1` then unlink `.xgc` — **NOT Corrupt** (authorized
//       Intent-gone lag after gates truly cleared).
//       Intent already gone + MAC-valid `.xgc` + leftover Started|C|A (or
//       PostSeal L/V/M) → **Corrupt** — MUST NOT “finish unlink `.xgc`
//       only”. Dual `.xgc` / MAC-fail / wrong-bind / disposition↔phase
//       mismatch / dual disposition mismatch / intent_mac mismatch / early
//       CREATE (phase not terminal for disposition OR physical gates still
//       present) / legacy v1 204B → **Corrupt**.
//   Orphan / dual traps (r67/r70/r71):
//     - `.x1` present for a build_nonce with no Intent AND no Mode-B-eligible
//       `.xgc` → Corrupt (forged / incomplete unauthorized GC).
//     - `.x1` present + no Intent + Mode-B-eligible `.xgc` → Mode B converge
//       (finish residual `.x1` + `.xgc`); NOT Corrupt.
//     - `.xgc` + leftover Started (Intent present or gone) / leftover C only
//       / leftover A under AbandonFinalizingClear → **Corrupt** (early `.xgc`;
//       CREATE is after C/A unlink — leftover gate means unauthorized early
//       write or incomplete capture order).
//     - Dual Intent still forbidden; leftover Intent + its `.x1` / `.xgc`
//       must finish cleanup before new Building CREATE.
//     - PreSeal-abandon / PostSeal / Abandon final Intent clear MUST use
//       **Normative GC order (mandatory, r67/r70/r71):**
//         1) CAPTURE DurableCleanupAuthEvidence while C/A still readable
//            (PostSeal/Abandon) into preallocated buffer (no yield) —
//            PreSeal: evidence zeros / Building-only rules;
//         2) unlink C/A (PostSeal/Abandon) → parent flush;
//         3) CREATE_NEW + fsync `.xgc` (file+parent) while Intent still
//            present, binding captured/reconstructed evidence
//            (CREATE-time PhysicalCleanupPreconditions hold);
//         4) TipExportProducerResume idempotently if pause armed;
//         5) keep `.xgc` present while unlink matching `.x1` one-by-one
//            (parent flush after each);
//         6) unlink Intent → parent flush;
//         7) unlink `.xgc` last → parent flush.
//       Forbidden: any `.x1` unlink without prior durable matching `.xgc`;
//       Forbidden: Intent unlink while matching `.x1` remain if `.xgc`
//       absent; Forbidden: Intent unlink before all matching `.x1` gone
//       even with `.xgc` (order is `.x1` then Intent then `.xgc`);
//       Forbidden: leave orphan `.x1` / leftover `.xgc` after Intent gone
//       without converging Mode B (only when Mode-B PhysicalCleanupPreconditions
//       hold; else early-`.xgc` Corrupt);
//       Forbidden: Mode B GC when Mode-B PhysicalCleanupPreconditions fail;
//       Forbidden: Mode B that requires live `.clr`/`.abd` or TipExport
//       "already executed" as a durable recovery fact.
//   Narrowed claim (r66, preserved): **Intent phase monotonic REPLACE alone
//   does NOT prove no-cross / no-skip after reboot.** Crash-verifiable path
//   proof is the `.x1` chain under HY-COMPINTENT-X-v1 whenever the candidate
//   is still mid-path (Mode A). Authorized GC gaps require verified `.xgc`
//   **plus** Mode-B PhysicalCleanupPreconditions from durable evidence (r70/r71).
//   Intent.phase remains the live/current discriminator after Mode A verify
//   succeeds or Mode B converge applies.

// CompactionCandidateIntent — normative phase moments (round-64/65/66/67):
//   Building: CREATE_NEW after pin CompactionSourceBaseline, BEFORE any
//     byte of gen-{target}/ is created. Binds baseline 4-tuple +
//     source/target generation + build_nonce. candidate_id=request_id=0.
//     **Genesis** — no CompactionIntentTransitionWire for none→Building.
//   Reserved: after durable SealIdWatermark advance + CREATE_NEW
//     SealJournalCommitWatermark for this candidate_id (same id-reserve
//     step as §10.1): publish `.x1` seq=1 Building→Reserved (bind nonzero
//     ids) → THEN REPLACE Intent. Crash after Reserved before Started →
//     journaled-but-not-Started / PreSeal-abandon (ids never reclaimed;
//     watermark already forward). Crash after `.x1` seq=1 before Intent
//     REPLACE → recovery one-step lag completes REPLACE to Reserved.
//   StartedPublished: after durable NativeV2Started or MigratedV2Started
//     publish: `.x1` seq=2 Reserved→StartedPublished → THEN REPLACE Intent.
//     Intent.phase=StartedPublished (after chain verify) is the sole live
//     proof that Started once existed for this build even if Started names
//     are later missing — **until** a terminal
//     Finalizing phase is raised.
//   PostSealFinalizing (r65/r66/r71): **immediately after** durable `.clr`
//     Authorized (PostSealCommittedProof holds; journal drain-complete;
//     CURRENT == new_generation) and **BEFORE unlinking any Started name
//     (M/V/L)**: publish `.x1` seq=3 StartedPublished→PostSealFinalizing →
//     THEN REPLACE Intent. Normative moment: after C Authorized durable →
//     `.x1` → Intent PostSealFinalizing → ordered unlink M→V→L → CAPTURE from
//     C → unlink C → CREATE `.xgc` (DurableCleanupAuthEvidence) →
//     TipExportProducerResume (idempotent) → GC `.x1` → clear Intent →
//     unlink `.xgc` last. Mid-clear may still have leftover C/M/V/L; Intent
//     Finalizing + CleanupInProgress coexist (presence≫`.clr` phase drives
//     unlink resume; Intent Finalizing + verified `.x1` chain proves terminal
//     PostSeal path; `.xgc` authorizes subsequent GC gaps **without** live C).
//   AbandonFinalizing (r65/r66/r67/r71): **after** ResumeAuthorized durable +
//     GenGone done, **while `.abd` still present**, and **BEFORE unlink A**:
//     publish `.x1` seq=3 StartedPublished→AbandonFinalizing → THEN REPLACE
//     Intent. Then CAPTURE from A → AbdPending/unlink A → CREATE `.xgc`
//     (DurableCleanupAuthEvidence) → TipExportProducerResume (idempotent) →
//     GC `.x1` → clear Intent → unlink `.xgc` last.
// Tip drift during Building (CURRENT tip ≠ Intent baseline): abandon
// Intent+G (PreSeal-abandon) — verify bridge/baseline vs Intent if G
// present; clean G; CREATE `.xgc` (PreSealAbandonClear; evidence zeros;
// terminal_transition_mac=zeros when no `.x1`) → TipExportProducerResume if
// pause armed (idempotent) → GC matching `.x1` (none expected at Building) →
// clear Intent → unlink `.xgc` last; restart from fresh baseline. Tip drift
// after Reserved with journals → B-sticky Corrupt rules unchanged;
// PreSeal-abandon under Reserved MUST CREATE `.xgc` then GC seq=1 `.x1`
// (and any further) before Intent unlink; `.xgc` last.

// PostSealCommittedProof (live checks vs C wire / Admitted Started):
//   1. CURRENT.generation == C.new_generation (== Started.new_generation)
//   2. LastRemoteAckedTip covers seal tip (round-58 sibling — NOT forever
//      byte-equal after N+1 live tip exports advance the breadcrumb):
//        tip.store_uuid == C.store_uuid;
//        (tip.generation, tip.sequence) >= (C.new_generation, C.new_final_seq)
//          lexicographic; tip.generation > C.new_generation → Corrupt/fence
//          (foreign compaction while Started live);
//        if tip.(generation,sequence) == (C.new_generation, C.new_final_seq)
//          → tip.tip_mac/key_id == C.new_final_tip_mac/new_key_id;
//        if forward within C.new_generation → mac/key are the advanced tip's
//          (seal tip already witnessed by monotonic CAS before flip).
//   3. gen-(C.new_generation) GenerationBridge binds
//        prev_* == C baseline 4-tuple AND new_generation/content_root path
//        consistent with Started (candidate_id/request_id/content_root)
//   4. journal drain-complete for C.candidate_id (no .sj1/.jhw/.jts)
// Missing ANY → proof fails. C wire still stores the seal new_* snapshot
// (immutable after Authorized); live tip may be equal-or-forward.

// Closed L↔V field-bind (round-57 P1) — ALL must hold; ONLY topology may differ:
//   L.store_uuid_lo/hi == V.store_uuid_lo/hi
//   L.candidate_id == V.candidate_id
//   L.source_generation == V.source_generation
//   L.baseline_tip_seq == V.baseline_tip_seq
//   L.baseline_tip_mac[32] == V.baseline_tip_mac[32]
//   L.baseline_key_id == V.baseline_key_id
//   L.new_generation == V.new_generation
//   L.new_final_seq == V.new_final_seq
//   L.new_final_tip_mac[32] == V.new_final_tip_mac[32]
//   L.new_key_id == V.new_key_id
//   L.request_id == V.request_id
//   L.content_root[32] == V.content_root[32]
// Forbidden to change any of the above during migrate. Topology
// (mask/count/ring_id[]) and V.kek_key_id are the only additive fields.

// Round-55…67 — Started wire disposition / v1→v2 migration (compatibility):
// Let L = seal-export-started, V = .v2, M = .mig, C = .clr, A = .abd,
//     I = compaction-candidate-intent (CompactionCandidateIntentWire);
//     X = compaction-intent-x-*.x1 (CompactionIntentTransitionWire; r66);
//     Gc = compaction-intent-gc-*.xgc (CompactionIntentGcAuthorizedWire; r67/r71 — live write v2/316B/`HY-COMPINTENT-GC-v2` + DurableCleanupAuthEvidence; legacy v1/204B fail-closed).
// | On-disk condition | Disposition |
// | I present (MAC-ok) ∧ phase∈{Building,Reserved} ∧ no admitted Started ∧ A absent |
// |   → **PreSealIntentAbandoned** — safe PreSeal-abandon (NOT Corrupt): verify
// |   bridge/baseline vs I if G present; GenGone rules for pre-Started (G absent
// |   without T legal; G present → no-replace to T optional or discard+bind);
// |   CREATE `.xgc` → TipExportProducerResume if pause armed (idempotent) →
// |   GC `.x1` → clear I → unlink `.xgc` last |
// | I present ∧ phase=PostSealFinalizing ∧ A absent ∧ **no** `.xgc` |
// |   → **PostSealIntentFinalizing** — NOT Corrupt: verify CURRENT==I.target /
// |   tip/bridge equal-or-forward, journal drain; if C present → resume
// |   CleanupInProgress (presence≫phase); finish leftover C/M/V/L; CAPTURE from
// |   C (or reconstruct if C already gone) → CREATE `.xgc` →
// |   TipExportProducerResume (idempotent) → GC `.x1` → clear I → unlink `.xgc`
// |   last. No Started + no C is legal (nearly-finished clear) |
// | I present ∧ phase=PostSealFinalizing ∧ `.xgc` present ∧
// |   (Started|C|L|V|M leftover) |
// |   → **Corrupt** — early `.xgc` (CREATE is after C unlink; leftover gate
// |   means unauthorized early write). MUST NOT Mode B GC |
// | I present ∧ phase=PostSealFinalizing ∧ `.xgc` v2 present ∧ L/V/M/C/Started
// |   all cleared ∧ Mode-B PhysicalCleanupPreconditions(PostSealFinalizingClear)
// |   from DurableCleanupAuthEvidence |
// |   → Mode B converge GC (partial `.x1` OK; no live C required) → clear I →
// |   unlink `.xgc` last |
// | I present ∧ phase=AbandonFinalizing ∧ **no** `.xgc` |
// |   → **AbandonIntentFinalizing** — NOT Corrupt: if A present → CAPTURE from A
// |   → unlink A → CREATE `.xgc` → TipExportProducerResume (idempotent) → GC
// |   `.x1` → clear I → unlink `.xgc`; if A absent → reconstruct evidence
// |   (bit4=1) → CREATE `.xgc` → TipExportProducerResume (idempotent) → GC
// |   `.x1` → clear I → unlink `.xgc` (never invent Corrupt solely for missing A) |
// | I present ∧ phase=AbandonFinalizing ∧ `.xgc` present ∧ (A|Started|C leftover) |
// |   → **Corrupt** — early `.xgc`; MUST NOT Mode B GC |
// | I present ∧ phase=AbandonFinalizing ∧ `.xgc` v2 present ∧ A/Started/C cleared ∧
// |   Mode-B PhysicalCleanupPreconditions(AbandonFinalizingClear) from
// |   DurableCleanupAuthEvidence |
// |   → Mode B converge GC (no live A required) → clear I → unlink `.xgc` last |
// | I absent ∧ `.xgc` present ∧ leftover Started|C|A (or PostSeal L/V/M) |
// |   → **Corrupt** — early `.xgc` / inconsistent; MUST NOT “finish unlink `.xgc`
// |   only” |
// | I absent ∧ `.xgc` v2 present ∧ Mode-B PhysicalCleanupPreconditions hold
// |   (+ residual `.x1` optional) → Mode B finish residual `.x1` + unlink `.xgc`
// |   (NOT Corrupt) |
// | I present ∧ phase=StartedPublished ∧ no admitted Started ∧ A absent |
// |   → **Corrupt/IoError** — Started cleared without A0 / missing `.abd` /
// |   missing terminal Finalizing phase (narrowed: NOT when phase∈
// |   {PostSealFinalizing, AbandonFinalizing}) |
// | I absent ∧ no Started ∧ A absent ∧ (G present OR T present) |
// |   → **Corrupt/IoError** — gen remnant without Intent (G written without
// |   Building Intent, or Intent cleared too early) |
// | A present (MAC-ok) ∧ abandon_reason=NotFound ∧ baseline binds Started |
// |   → **AbandonInProgress** — resume ClrAbandoned (GenGone → ResumeAuthorized →
// |   Intent AbandonFinalizing → CAPTURE from A → unlink A → CREATE `.xgc` →
// |   TipExportProducerResume (idempotent) → GC `.x1` → clear I → unlink `.xgc`);
// |   (includes no-C A0: present_mask.C=0 + digest_C=0; with-C: bit3=1 + digest_C);
// |   keep I until A4 complete then `.xgc`-authorized clear; FORBIDDEN PreSeal /
// |   new id reserve / CleanupInProgress / reverse to Found |
// | A present ∧ (MAC-fail / reason≠NotFound / PostSealCommittedProof also holds
// |   / CURRENT already == Started.new_generation) → **Corrupt** |
// | C present (v2 304B MAC-ok) ∧ A absent ∧ PostSealCommittedProof holds ∧
// |   started_kind∈{NativeV2,MigratedV2} ∧ kind/digests match leftovers |
// |   → **CleanupInProgress** — resume fixed unlink; FORBIDDEN PreSeal /
// |   new id reserve / Corrupt solely for leftover L/V/M |
// | C present (MAC-ok or draft 176B) ∧ A absent ∧ PostSealCommittedProof FAILS |
// |   → **ClrUnauthorized** — NOT CleanupInProgress; do NOT unlink Started;
// |   classify L/V/M as NativeV2/MigratedV2/Legacy…; force query-first PostSeal |
// | C present ∧ (LegacyStarted / incomplete-mig / kind mismatch / digests
// |   contradict remaining files) → **Corrupt** — never unlink via bad C |
// | C size==176 (draft r57) | Corrupt / ClrUnauthorized (never pad to 304) |
// | L is v2 (238B) complete MAC-ok under L.kek_key_id + topology; V/M/C absent |
// |   → **NativeV2Started** PostSeal; freeze from L |
// | L is v2 damaged / topology fail / kek wrapper missing | Corrupt / fence |
// | L size==234 (draft) | Corrupt / fence (never pad to 238) |
// | L is legacy 192B MAC-ok under explicit legacy_kek; V+M+C absent |
// |   → **LegacyStarted** — hard fence; NOT PostSeal; NOT PreSeal; NOT missing |
// | L legacy MAC-ok ∧ V v2 MAC-ok ∧ closed L↔V equalities ∧ M v2 (208B) MAC-ok
// |   ∧ M digests/macs/kek ids bind L+V ∧ C absent |
// |   → **MigratedV2Started** PostSeal; freeze from V |
// | L legacy ∧ V present ∧ M absent/torn | still LegacyStarted fence |
// | M size==136 (draft) / M without matching L+V / digest/mac/id/kek mismatch |
// |   → Corrupt / fence |
// | L legacy MAC fail under supplied kek | Corrupt / fence (not torn ignore) |
// | other lengths on L | torn/Corrupt — never pad/truncate; never default mask=0 |
// Offline migration (operator tool only — never hot-path auto-upgrade) —
// **companion strategy (round-56/57); REPLACE / delete-legacy WITHDRAWN:**
//   0. Preconditions: L is LegacyStarted (192B). Do **not** unlink L.
//   1. Operator supplies **exactly one** `legacy_kek_key_id`; verify L under
//      KEK[legacy_kek_key_id] only — fail-closed if missing/fail (no try-all /
//      no current-key guess). Copy closed field set into V unchanged.
//   2. Require explicit operator-supplied topology (same freeze rules);
//      refuse default/empty mask. Set V.kek_key_id = active journal KEK id
//      (may equal legacy_kek_key_id).
//   3. Encode V (238B) → `*.v2.tmp` → fsync → CREATE_NEW/no-replace V →
//      parent flush. If V exists: byte-equal → idempotent; else Corrupt.
//   4. Compute SHA-256 digests of full L and V file bytes; encode M (208B)
//      binding digests + trailer macs + both kek ids + ids → CREATE_NEW M →
//      parent flush (byte-equal idempotent).
//   5. Only when L+V+M all verify → MigratedV2Started.
// Clear Started (round-57/58/65/66) — AFTER PostSeal committed AND journal drain:
//   A. CREATE_NEW C (phase=Authorized, format_version=2, 304B) **only** when:
//      - disposition is NativeV2Started or MigratedV2Started (never Legacy /
//        incomplete-mig);
//      - **PostSealCommittedProof** holds NOW (CURRENT flipped to
//        Started.new_generation; LastRemoteAckedTip field-matches Started
//        new_*; gen-N+1 GenerationBridge binds Started baseline/ids/content_root;
//        journal drain-complete — no .sj1/.jhw/.jts);
//      - copy proof fields + digests + kind/mask from admitted Started;
//        kek_key_id == Started wire kek (L.NativeV2 / V.MigratedV2).
//      Forbidden: CREATE C before tip persist / before CURRENT flip / before
//        bridge durable / while journal remnants remain (wrongful early .clr).
//      Forbidden: CREATE C to “clear” LegacyStarted.
//      If unauthorized C (ClrUnauthorized / draft 176) already occupies the
//        name: after proof becomes true, REPLACE once to Authorized v2 with
//        full proof (watermark REPLACE class) — never unlink Started under
//        the old unauthorized bytes. (NotFound abandon uses ClrAbandoned below
//        — never silent unlink of unauthorized C.)
//   A2. **Intent→PostSealFinalizing (round-65/66) — BEFORE any Started unlink:**
//      Immediately after C Authorized is durable (CREATE_NEW or proof-valid
//      REPLACE): publish `.x1` seq=3 StartedPublished→PostSealFinalizing
//      (file+parent flush) → THEN REPLACE Intent phase→PostSealFinalizing
//      (recompute MAC; parent flush). Require Intent was StartedPublished with
//      matching candidate_id/request_id/baseline + verified prior `.x1`
//      chain (seq 1..2). Forbidden: unlink any of M/V/L before Intent
//      PostSealFinalizing is durable; Forbidden: Intent REPLACE without
//      matching `.x1`; Forbidden: raise PostSealFinalizing from
//      Building|Reserved or while proof fails / journal remnants remain.
//   B. Fixed unlink order + parent flush after each unlink; then REPLACE C
//      to raise phase only (proof fields immutable):
//        next_name = first still-present name in order M → V → L whose
//        present_mask bit is set (NativeV2 typically mask=L only → skip M/V);
//        unlink → phase MGone/VGone/LGone. Idempotent if name already gone.
//      Intent stays PostSealFinalizing through the entire unlink (CleanupInProgress
//      `.clr` presence≫phase coexists with Intent Finalizing).
//   C. When no L/V/M remain: phase=ClrPending → **CAPTURE** DurableCleanupAuthEvidence
//      from C (mac + PostSealCommittedProof fields + journal-drain) into
//      preallocated buffer (no yield) → unlink C → parent flush → **CREATE
//      `.xgc` (PostSealFinalizingClear; bind captured evidence)** →
//      TipExportProducerResume for gen-N+1 (**idempotent**; not Mode B durable
//      auth) → GC matching `.x1` → **clear Intent** → unlink `.xgc` last.
//   D. Only then may new id reserve / PreSeal admit.
//   Crash after last Started/C gone but before `.xgc`/Intent clear: recovery
//      sees I.phase=PostSealFinalizing + no Started + no A → reconstruct
//      evidence (gate_absent_at_create=1) → CREATE `.xgc` if absent →
//      TipExportProducerResume idempotent → GC → clear I → unlink `.xgc`
//      (**NOT Corrupt**).
//   Crash mid-`.x1` GC with durable `.xgc` + Mode-B PhysicalCleanupPreconditions
//   (from `.xgc` evidence + live gate absence): Mode B converge (NOT false
//   Corrupt; does **not** need live C). `.xgc` + leftover Started/C/A →
//   Corrupt (early `.xgc`; MUST NOT Mode B). Must-pass: `C deleted → .xgc
//   written → delete any x1 → crash` → Mode B without live C.
// ClrAbandoned (round-59…63 P0) — EVERY authenticated-NotFound Started abandon
//   (with C OR without C) — converges ClrUnauthorized + NotFound abandon:
//   Preconditions (ALL required; else refuse / keep ClrUnauthorized / Corrupt):
//     - Disposition ClrUnauthorized (C present; PostSealCommittedProof fails)
//       OR admitted NativeV2/MigratedV2 Started with no C (authenticated NotFound)
//       OR recovery already sees MAC-ok A (resume);
//     - authenticated SealQueryStatus::NotFound for Started.request_id
//       (TransportUnavailable → NEVER write A; fence + re-query);
//     - CURRENT.generation == Started.source_generation (not flipped);
//     - N tip == Started/bridge baseline 4-tuple; else Corrupt;
//     - journal empty for candidate (no .sj1/.jhw/.jts);
//     - admitted NativeV2/MigratedV2 Started still present at A0
//       (except recovery resume when A already present and Started already
//       unlinked under presence≫phase);
//     - PostSealCommittedProof does NOT hold (else CleanupInProgress path).
//   Let new_generation = Started.new_generation (== source_generation+1 for
//     legal compaction; UINT32_MAX refused earlier).
//     G = store `gen-{new_generation}/`.
//     T = store `abandoned/gen-{new_generation}-c{candidate_id}/`.
//   Order (durable; parent flush after each step; presence≫stale phase):
//     A0. CREATE_NEW A (phase=Authorized, 192B) binding ids/baseline/content_root/
//         started_kind/present_mask/digest_C/abandon_reason=NotFound under
//         KEK[kek_key_id] — **before** any unlink of C, Started, or G.
//         **No-C case (round-63 P0):** present_mask bit3(C)=0 and digest_C[32]=0
//         (all-zero); with-C: bit3=1 and digest_C=SHA-256(unauthorized C bytes).
//         Snapshot new_generation from Started into owner RAM for GenGone
//         (wire binds content_root + baseline; compaction new_g = source+1).
//     A1. Unlink unauthorized C → phase=CGone (idempotent if already gone;
//         **no-op when present_mask.C=0** — A0 already recorded C absent).
//     A2. Unlink Started names in order M → V → L (skip clear bits) with phase
//         MGone/VGone/LGone — same presence≫phase rule as `.clr`.
//     A2b. **GenGone (round-61/62 P1)** — admitted-Started abandon path:
//         1) If G present: verify GenerationBridge.prev_* == A baseline
//            4-tuple and sealed content_root(G) == A.content_root; else Corrupt.
//            Durable **no-replace** rename G → T (CREATE `abandoned/` if needed;
//            if T already exists: bind-verify bridge/content_root/baseline/
//            candidate_id — match ⇒ idempotent; mismatch ⇒ Corrupt). Then
//            **parent fsync**. Forbidden: REPLACE into a live gen name.
//         2) If G absent and T present: bind-verify T as above → GenGone
//            idempotent (rename succeeded; parent flush may have been pending).
//         3) If G absent and T absent: **Corrupt / IoError** on this path
//            (admitted Started implies G was created+flushed before Started;
//            abnormal loss must not be swallowed). **Not** the PreSeal-abandon
//            branch (no Started yet), where G may be absent without T.
//         4) Optional recoverable delete of G only after T is durable and
//            bind-verified (prefer rename; delete-without-T forbidden here).
//         → phase=GenGone. Hot-path: directory rename/delete only; no heap
//            payload walks.
//     A3. **ResumeAuthorized (round-62 P1) — `.abd` STILL PRESENT:**
//         Re-verify CURRENT.generation == A.source_generation and live tip ==
//         A baseline 4-tuple (fields read from on-disk A). REPLACE A phase →
//         ResumeAuthorized + recompute MAC + parent flush.
//         **Producer remains paused** through A3 (tip must not advance while
//         A still gates abandon). Forbidden: unlink A before ResumeAuthorized
//         flush; Forbidden: TipExportProducerResume before unlink A on this
//         path (resume-before-unlink ⇒ tip may leave baseline while A remains
//         ⇒ recovery tip==baseline self-lock).
//         Must-pass: pause → Started → NotFound → A0…A3 → A still present +
//         tip==baseline + producer paused.
//     A3a. **Intent→AbandonFinalizing (round-65/66) — `.abd` STILL PRESENT:**
//         After A3 (ResumeAuthorized) + GenGone done: publish `.x1` seq=3
//         StartedPublished→AbandonFinalizing (file+parent flush) → THEN
//         REPLACE Intent→AbandonFinalizing (recompute MAC; parent flush)
//         **while A is still on disk**. Require Intent was StartedPublished
//         with matching ids/baseline + verified prior `.x1` chain (seq 1..2).
//         Forbidden: unlink A before Intent AbandonFinalizing durable;
//         Forbidden: Intent REPLACE without matching `.x1`; Forbidden: raise
//         AbandonFinalizing from Building|Reserved / PostSealFinalizing /
//         without ResumeAuthorized (illegal 3→4 cross is also Corrupt via
//         `.x1` chain / Intent.phase>last.to without edge).
//     A3b. Monotonic REPLACE A phase=AbdPending + parent flush → unlink A →
//         parent flush. Only after A3+A3a. (A phase raise may coalesce with
//         A3's ResumeAuthorized REPLACE only if Intent AbandonFinalizing is
//         already durable and unlink follows in the same crash-free critical
//         section; recovery still accepts either cut.)
//     A3c. **CAPTURE** DurableCleanupAuthEvidence from A **before unlink**
//         (A.mac at phase≥ResumeAuthorized, content_root, started_kind,
//         present_mask, gen_gone + resume_authorized flags) into preallocated
//         buffer (no yield). Then A3b unlink A (if not already in same
//         critical section after capture).
//     A4. **CREATE `.xgc` (AbandonFinalizingClear)** binding captured evidence
//         (Intent still AbandonFinalizing) → parent flush →
//         **TipExportProducerResume** `try_push` for CURRENT (gen-N) —
//         authorized by durable ResumeAuthorized evidence now bound in `.xgc`
//         / previously observed on A (**never** re-read unlinked A; resume is
//         **idempotent**, not a Mode B "already occurred" precondition) →
//         GC matching `.x1` → clear Intent → unlink `.xgc` last. Then new id
//         reserve / PreSeal admit / greenfield Started.
//         Must-pass: A0…A3a → capture → unlink A → CREATE `.xgc` → resume →
//         GC `.x1` → clear I → unlink `.xgc` → try_push → remote tip advances;
//         `A deleted → .xgc written → delete any x1 → crash` → Mode B without
//         live A; power-cut after A3 before A3a → recovery raises
//         AbandonFinalizing then capture/unlink; power-cut after A3a before
//         unlink → I.AbandonFinalizing + A present → capture + unlink + CREATE
//         `.xgc` → resume → GC → clear I → unlink `.xgc`; power-cut after A
//         unlink before CREATE `.xgc` → A absent + I.AbandonFinalizing →
//         reconstruct evidence (bit4=1) → CREATE `.xgc` → TipExportProducerResume
//         idempotent → GC → clear I → unlink `.xgc` (**NOT Corrupt**).
//   Id rule: SealIdWatermark must already be past abandoned candidate_id/
//     request_id; **never** reuse those ids (forward-only watermark only).
//   Forbidden: unlink unauthorized C / Started on NotFound without prior durable A
//     (covers with-C and no-C — no side-path that clears Started then GenGone);
//     Forbidden: CREATE A on TransportUnavailable / Found / tip≠baseline /
//     journal non-empty / CURRENT already at new_generation;
//     Forbidden: reverse AbandonInProgress because a later query returns Found
//     (authenticated NotFound + durable A ⇒ committed abandon; later Found →
//     Corrupt/equivocation fence — not PreSeal reopen; same for no-C path);
//     Forbidden: unlink A while G still live or phase < ResumeAuthorized;
//     Forbidden: TipExportProducerResume without durable ResumeAuthorized
//     (and, on ClrAbandoned path, without A already unlinked or AbdPending
//     completion in progress);
//     Forbidden: GenGone idempotent success when G and T both absent after
//     admitted Started;
//     Forbidden: resume-before-unlink that leaves tip≠A.baseline while A live.
// Recovery:
//   0. Discard only *.clr.tmp / *.abd.tmp (torn). Verify C under C.kek_key_id
//      (never try-all). Draft 176 / MAC-fail → ClrUnauthorized or Corrupt
//      (no unlink). Kind/digest/Legacy mismatch → Corrupt.
//   0b. If A present (MAC-ok): **AbandonInProgress** — resume from phase:
//      GenGone incomplete → A2b; else if phase < ResumeAuthorized → A3;
//      else if ResumeAuthorized → re-verify tip==baseline from A (producer
//      must still be treated as paused) → ensure Intent AbandonFinalizing
//      (A3a) → CAPTURE from A → A3b unlink → A4 CREATE `.xgc` →
//      TipExportProducerResume (idempotent) → GC `.x1` → clear I → unlink `.xgc`;
//      else if AbdPending → ensure Intent AbandonFinalizing → CAPTURE if A
//      still readable → unlink A → A4 CREATE `.xgc` → TipExportProducerResume
//      (idempotent) → GC `.x1` → clear I → unlink `.xgc`.
//      If ResumeAuthorized but tip already equal-or-forward of A.baseline
//      (non-compliant resume-before-unlink residue): ensure producer resumed
//      idempotently, complete A3a/A3b unlink, **do not** require tip byte-equal
//      (else self-lock); tip behind baseline / wrong generation → Corrupt.
//      FORBIDDEN query-first reopen / CleanupInProgress / PreSeal / new reserve
//      / greenfield Started while A present.
//   0c. If A absent: if I.phase==AbandonFinalizing → reconstruct
//      DurableCleanupAuthEvidence (gate_absent_at_create=1) → CREATE `.xgc`
//      if absent → TipExportProducerResume (idempotent) → GC `.x1` → clear I
//      → unlink `.xgc` (nearly-finished abandon; NOT Corrupt). Else
//      TipExportProducerResume only when no live PostSeal/abandon gate remains
//      (no Started/C/CleanupInProgress/ClrUnauthorized/non-terminal Intent)
//      and CURRENT/tip is consistent with the last abandoned baseline if an
//      abandoned T exists for that candidate; never invent ResumeAuthorized
//      without A unless I.phase==AbandonFinalizing / `.xgc` evidence binds it.
//      Power-cut after A unlink before/after CREATE `.xgc` is OK (CREATE +
//      resume + Intent clear idempotent under AbandonFinalizing).
//   0d. Intent-gated remnant classification (round-64/65/66/67 — narrows r63/r64):
//      Load/verify I under wire kek_key_id (never try-all). Discard only
//      `*.intent.tmp` / `*.x1.tmp` / `*.xgc.tmp` / short torn Intent (incomplete
//      length). Complete MAC-valid Intent ⇒ Intent present (Windows parent-
//      flush undecidable). Complete MAC-valid `.xgc` ⇒ GC auth present.
//      **Transition chain + GC auth (r66 P1 / r67 P0 / r70/r71 P0 — before
//      trusting I.phase path):** Load optional
//      `compaction-intent-gc-<I.build_nonce>.xgc` (and any `.xgc` whose
//      build_nonce has no Intent). Legacy v1 204B → **Corrupt**/fail-closed.
//      If `.xgc` v2 MAC-ok + bind/disposition/intent_mac/DurableCleanupAuthEvidence
//      legal **AND Mode-B PhysicalCleanupPreconditions(terminal_disposition)
//      hold** (from `.xgc` evidence + live gate absence — **never** re-read
//      deleted `.clr`/`.abd`; TipExportProducerResume is idempotent after auth,
//      not an "already occurred" precondition) → **Mode B** converge GC
//      (partial/gapped `.x1` OK; Intent-gone + `.xgc` + gates cleared → finish
//      residual `.x1` + `.xgc`; forged/dual/wrong-bind/dual-disposition /
//      early `.xgc` with leftover Started|C|A|L|V|M → **Corrupt**, MUST NOT
//      Mode B). Else if `.xgc` MAC-ok but Mode-B PhysicalCleanupPreconditions
//      fail → **Corrupt** (early/forged `.xgc`; do **not** fall through to
//      finish CleanupInProgress as if `.xgc` were absent). Else **Mode A**:
//      load all `compaction-intent-x-<I.build_nonce>-*.x1`; verify contiguous
//      seq 1..N; filename==wire; prev_mac chain; only legal edges; binds == I.
//      Orphan `.x1` (no Intent, no Mode-B-eligible `.xgc`) → Corrupt.
//      Intent.phase vs T*: equal to_phase OK; equal from_phase → complete
//      REPLACE (one-step lag); phase ahead without `.x1` → Corrupt; no `.x1`
//      ⇒ Building OK, else (phase>Building without Mode-B-eligible `.xgc`) →
//      Corrupt — r66 GC-lag exception **withdrawn**. Missing/jump/cross/gap/
//      prev_mac break without Mode-B-eligible `.xgc` → Corrupt. Do **not**
//      treat replaceable I.phase alone as proof of no-cross. Do **not** treat
//      “MAC-valid `.xgc`” alone as Mode B eligibility (r70/r71).
//      - I.phase ∈ {Building, Reserved} ∧ A absent ∧ no admitted Started ∧
//        **no** `.xgc`:
//        **PreSeal-abandon** (NOT Corrupt) — verify bridge.prev_* /
//        baseline vs I if G present; GenGone for pre-Started (G absent
//        without T legal; if G present no-replace→T or discard+bind);
//        journaled-but-not-Started under Reserved → replay-to-N then cleanup;
//        clear I only after that cleanup + TipExportProducerResume if pause
//        armed + CREATE `.xgc` → GC `.x1` → Intent → `.xgc` last. Path-A
//        PreSeal-abandon still does **not** require `.abd`.
//      - I.phase ∈ {Building, Reserved} ∧ `.xgc` present ∧ leftover
//        Started|A|C (or PreSeal cleanup incomplete) → **Corrupt** (early
//        `.xgc`); if PhysicalCleanupPreconditions(PreSealAbandonClear) hold
//        → Mode B converge.
//      - I.phase == PostSealFinalizing ∧ A absent ∧ **no** `.xgc`:
//        **PostSealIntentFinalizing** (NOT Corrupt) — verify CURRENT ==
//        I.target_generation, tip/bridge equal-or-forward covers seal tip,
//        journal drain-complete; if C present resume CleanupInProgress
//        (presence≫phase; leftover C/M/V/L OK) then CAPTURE→unlink C→CREATE
//        `.xgc`; if C absent reconstruct evidence (bit4=1) → CREATE `.xgc`;
//        TipExportProducerResume (idempotent) → GC `.x1` → clear I → unlink
//        `.xgc` last. No Started + no C under this phase is legal.
//      - I.phase == PostSealFinalizing ∧ `.xgc` ∧ leftover Started|C|L|V|M →
//        **Corrupt** (early `.xgc`); gates cleared + Mode-B
//        PhysicalCleanupPreconditions from DurableCleanupAuthEvidence → Mode B.
//      - I.phase == AbandonFinalizing ∧ **no** `.xgc`:
//        **AbandonIntentFinalizing** (NOT Corrupt) — if A present CAPTURE →
//        unlink A → CREATE `.xgc` → TipExportProducerResume (idempotent) →
//        GC `.x1` → clear I → unlink `.xgc`; if A absent → reconstruct
//        (bit4=1) → CREATE `.xgc` → TipExportProducerResume (idempotent) →
//        GC `.x1` → clear I → unlink `.xgc`.
//      - I.phase == AbandonFinalizing ∧ `.xgc` ∧ leftover A|Started|C →
//        **Corrupt** (early `.xgc`); gates cleared + Mode-B
//        PhysicalCleanupPreconditions from DurableCleanupAuthEvidence → Mode B.
//      - I.phase == StartedPublished ∧ A absent ∧ no admitted Started:
//        **Corrupt/IoError** — proof of prior Started without A0 / without
//        live Started names / without terminal Finalizing phase (Started
//        cleared without `.abd` and without PostSealFinalizing).
//      - I absent ∧ A absent ∧ no Started ∧ (G present OR T present):
//        **Corrupt/IoError** — remnant without Intent (not legal PreSeal).
//      - I absent ∧ `.xgc` ∧ leftover Started|C|A → **Corrupt** (not finish
//        `.xgc` only).
//      - I present ∧ (`.x1` Mode-A chain fail / phase regression / Intent
//        REPLACE without matching `.x1` / illegal PostSeal↔Abandon cross —
//        including MAC-valid I.phase=AbandonFinalizing after true
//        PostSealFinalizing path with no 2→4 edge / Building|Reserved→
//        *Finalizing jump / Reserved with candidate_id==0 /
//        StartedPublished|*Finalizing with ids==0 / baseline mismatch vs
//        bridge when G present / orphan `.x1` without Intent and without
//        Mode-B-eligible `.xgc` / forged or wrong-bind `.xgc` / dual `.xgc` /
//        dual disposition mismatch / early `.xgc` CREATE / `.xgc` + leftover
//        gates) → **Corrupt**.
//      Distinct from Path-A PreSeal-abandon under Building|Reserved (never
//      wrote Started): that branch does **not** require `.abd`.
//   1. Evaluate PostSealCommittedProof against live CURRENT +
//      LastRemoteAckedTip + bridge + journal. If FAILS → **ClrUnauthorized**:
//      keep Started; **FORBIDDEN** CleanupInProgress unlink; force
//      query-first PostSeal (step NativeV2/MigratedV2). Must-pass: Started +
//      zero journal + wrongful .clr + remote Found → query Found → tip/flip/
//      drain → only then authorize/replace C (never generation fork).
//   2. If proof HOLDS → CleanupInProgress: presence≫stale phase; raise phase;
//      if Intent still StartedPublished → raise PostSealFinalizing before
//      further Started unlinks (or immediately if mid-clear already past A2);
//      resume B–C; never PreSeal / never Corrupt solely for leftovers.
// Forbidden: unlink L/V/M without proof-valid C (PostSeal clear) or without
// durable A (any authenticated-NotFound Started abandon, with or without C);
// Forbidden: invent empty topology;
// Forbidden: try-all KEK; Forbidden: pad draft 234/136/176 wires;
// Forbidden: `.clr` as LegacyStarted eraser; Forbidden: CleanupInProgress when
// CURRENT still at source_generation / tip not at new_* / bridge unbound;
// Forbidden: ClrUnauthorized self-lock (NotFound abandon that leaves C);
// Forbidden: ClrAbandoned/NotFound/Path-A abandon that leaves tip-export
// producer paused; Forbidden: ClrAbandoned that leaves live gen-N+1/;
// Forbidden: use-after-unlink of A for TipExportProducerResume;
// Forbidden: ClrAbandoned resume-before-unlink / tip-advance self-lock;
// Forbidden: GenGone false idempotency (G+T both missing after Started);
// Forbidden: NotFound-without-C clear Started without prior durable A;
// Forbidden: treat StartedPublished+no-Started+no-A as PreSeal;
// Forbidden: treat PostSealFinalizing/AbandonFinalizing+no-Started(+no-A) as Corrupt;
// Forbidden: unlink last Started / clear C before Intent PostSealFinalizing;
// Forbidden: unlink A before Intent AbandonFinalizing;
// Forbidden: PostSealFinalizing↔AbandonFinalizing cross / Building|Reserved→Finalizing;
// Forbidden: Intent phase REPLACE without prior durable matching `.x1`;
// Forbidden: REPLACE / overwrite of live `.x1` transition receipts;
// Forbidden: treat Intent phase monotonic REPLACE alone as crash-verifiable
// no-cross / no-skip proof (r66 — require `.x1` chain);
// Forbidden: unlink any `.x1` without prior durable matching `.xgc`;
// Forbidden: CREATE `.xgc` before terminal cleanup authorizes Intent clear /
// while Started|A|C (or PostSeal L/V/M) still gate cleanup / wrong disposition
// vs Intent.phase / without DurableCleanupAuthEvidence capturable or
// reconstructible; Forbidden: yield between capture and CREATE on live path;
// Forbidden: Mode B that re-reads deleted `.clr`/`.abd` or requires
// TipExportProducerResume "already executed" as durable recovery fact;
// Forbidden: Mode B converge GC when Mode-B PhysicalCleanupPreconditions fail
// (early `.xgc` — including Intent-gone + leftover Started|C|A);
// Forbidden: treat intent_phase_at_auth / MAC-valid `.xgc` alone as Mode B
// eligibility without Mode-B PhysicalCleanupPreconditions (r70/r71);
// Forbidden: admit legacy v1 204B / HY-COMPINTENT-GC-v1 as Mode B;
// Forbidden: REPLACE / overwrite of live `.xgc`; dual `.xgc`;
// Forbidden: clear Intent while matching `.x1` remain (even with `.xgc` —
// order is `.x1` then Intent then `.xgc`);
// Forbidden: Intent unlink before all matching `.x1` gone if `.xgc` absent;
// Forbidden: leave orphan `.x1` after Intent unlink without Mode-B-eligible
// `.xgc` converge; Forbidden: leave leftover `.xgc` after Intent gone without
// finishing unlink (only when PhysicalCleanupPreconditions hold; else Corrupt);
// Forbidden: treat bare G without Intent as PreSeal;
// Forbidden: write G before durable Building Intent;
// Forbidden: dual/leftover Intent (incl. *Finalizing) blocking without prior cleanup;
// Forbidden: Intent phase regression / clear Intent before final cleanup;
// Forbidden: raise Reserved/StartedPublished/PostSealFinalizing/AbandonFinalizing out of order.

// Round-39…46 P0 — exactly-once seal-journal APPLY as ONE durable frame.
// Round-39 two-phase marker WITHDRAWN. Round-40 single-frame retained.
// Round-41: de-dup INDEX KEY must NOT include entry_mac (else "different MAC
// → Corrupt" is unreachable — different MAC looks like a new key → double
// apply). Index = {candidate_id, journal_seq}.
// Round-46: entry_mac is NOT opaque-only — EVERY apply MUST recompute
// HY-SEALJRN-v1 then compare; see SealJournalAppliedView.
// Round-41: call ABI is SealJournalAppliedView (span), not a payload-less
// struct — otherwise append cannot see business bytes.

struct SealJournalOriginKey {
    std::uint64_t candidate_id{0};
    std::uint64_t journal_seq{0};
    // entry_mac is NOT part of this key — see SealJournalAppliedView.
};

// Round-42 P1 — closed allowlist for seal-journal / SealJournalApplied embed.
// Anything else (GenerationBridge, OperatorOverride, SealJournalApplied,
// compaction-only snapshots, registry/weight/usage, TransportFailover) is
// NOT embeddable — journal admit and apply MUST Corrupt / refuse.
constexpr bool is_seal_journal_embeddable_type(DurableRecordType t) noexcept {
    switch (t) {
        case DurableRecordType::OrderEvent:
        case DurableRecordType::OrderCheckpoint:
        case DurableRecordType::RateLimitFreeze:
        case DurableRecordType::FreezeEpochWatermark:
        case DurableRecordType::FreezeProbeAttempt:
        case DurableRecordType::FreezeClear:
        case DurableRecordType::FreezeWaitArm:
        case DurableRecordType::FreezeWaitSatisfied:
            return true;
        default:
            return false;
    }
}

// ── Seal-journal on-disk wire (round-46…50) — packed little-endian, NO padding,
// NO alignment holes, NO platform-dependent sizeof. One published file per
// entry under the breadcrumb journal dir:
//   seal-journal/<store_uuid_lo_hex16><store_uuid_hi_hex16>/
//     <candidate_id_hex16>-<journal_seq_hex16>.sj1
// Publish recipe (round-47…50 — immutable final; NEVER §10.3 REPLACE for finals):
//   0. Encode into a preallocated buffer ≤ kSealJournalMaxEntryBytes (stack or
//      owner pool — NO hot-path heap / NO std::string growth).
//   1. Exclusive-create <same>.sj1.tmp → write buffer → fsync/FlushFileBuffers(tmp).
//   2. NO-REPLACE publish to final `.sj1` (created_final = true on success):
//        POSIX (preferred): linkat(tmp→final) then fsync(parent); unlink(tmp)
//          OR renameat2(RENAME_NOREPLACE) then fsync(parent).
//        Windows NTFS-local (PREFERRED): CreateHardLinkW(final ← tmp)
//          (fails if exists / not same volume) → FlushFileBuffers(parent dir)
//          → DeleteFileW(tmp). Hardlink is zero-copy and cannot tear the final.
//        Windows FALLBACK only if hardlink returns ERROR_NOT_SAME_DEVICE /
//          ERROR_INVALID_FUNCTION / equivalent: CreateFileW(CREATE_NEW) →
//          write full buffer → FlushFileBuffers(final) → FlushFileBuffers(parent)
//          → DeleteFileW(tmp). Mid-copy short final: if seq==commit_hw+1 and no
//          higher evidence, discard; if seq<=commit_hw → Corrupt.
//        FORBIDDEN for `.sj1`/`.jts`: MoveFileExW(REPLACE_EXISTING),
//          std::filesystem::rename that replaces, POSIX rename without NOREPLACE.
//   3. If final already exists: open + complete-length + HY-SEALJRN-v1 verify
//      into the same ≤MaxEntry buffer; if byte-equal to candidate → idempotent
//      (discard tmp, do NOT rewrite); else Corrupt (original unchanged).
//   4. Parent already flushed in step 2 on create path; on idempotent path still
//      FlushFileBuffers(parent) once → load prev_hw → durable-replace `.jhw`
//      (`.jhw` MAY use §10.3 REPLACE + monotonic CAS — watermark class, not a
//      final) → counter adjust (round-49/50):
//        journal_bytes_used += total_bytes AND journal_entry_count += 1
//        IFF (created_final) OR (byte-equal AND prev_hw < journal_seq);
//        else skip both. Only then Ack.
// Crash before successful no-replace + parent flush ⇒ only .tmp (and maybe
// short fallback final) — never Acked. Final .sj1 is committed evidence.
// Complete length: file_size == total_bytes == kSealJournalFixedMetaBytes + payload_len.
// file_size > total_bytes after a MAC-valid declared span → Corrupt (round-47).
// Durability claim: same honesty as §10.3 — process-kill / clean shutdown under
// the flush recipe; full power-cut equivalence only after fault-inject passes.
//
// SealJournalEntryWire layout (offsets absolute from file start):
//   +0   u32 format_version          (= kSealJournalFormatVersion = 1)
//   +4   u32 total_bytes             (= 138 + payload_len)
//   +8   u64 store_uuid_lo
//   +16  u64 store_uuid_hi
//   +24  u32 kek_key_id
//   +28  u64 candidate_id
//   +36  u64 journal_seq             (per candidate: starts at 1, strict +1)
//   +44  u32 source_generation
//   +48  u64 baseline_tip_seq
//   +56  u8  baseline_tip_mac[32]
//   +88  u32 baseline_key_id
//   +92  u8  time_kind               (FrameTimeKind)
//   +93  i64 recorded_utc_ms
//   +101 u8  record_type             (DurableRecordType; embeddable only)
//   +102 u32 payload_len             (0..kSealJournalMaxEmbeddedBytes)
//   +106 u8  payload_bytes[payload_len]
//   +106+payload_len  u8 entry_mac[32]
//
// Derived constants (MUST match layout; static_assert / encode-length tests):
constexpr std::uint32_t kSealJournalFormatVersion = 1;
constexpr std::size_t kSealJournalFixedMetaBytes = 138;   // bytes excl. payload
constexpr std::size_t kSealJournalMaxEmbeddedBytes = 4096;
constexpr std::size_t kSealJournalMaxEntryBytes =
    kSealJournalFixedMetaBytes + kSealJournalMaxEmbeddedBytes; // 4234
// kSealJournalMaxBytes = kSealJournalMaxEntries * kSealJournalMaxEntryBytes
//   (defined with MaxEntries in §10.1 step 5).
//
// entry_mac domain (committed marker == this ONE MAC; LE, no padding).
// Round-47: format_version + total_bytes ARE in the MAC domain.
//   HMAC(KEK[kek_key_id], "HY-SEALJRN-v1" || format_version || total_bytes ||
//     store_uuid_lo || store_uuid_hi || kek_key_id || candidate_id ||
//     journal_seq || source_generation || baseline_tip_seq ||
//     baseline_tip_mac || baseline_key_id || time_kind || recorded_utc_ms ||
//     record_type || payload_len || payload_bytes)
// Round-43/44/46/47: time + baseline + kek_key_id + framing are FIRST-CLASS.
// Round-46 P0: "optionally recomputed" / opaque-only comparison is WITHDRAWN.
// Round-47 P0: silent discard of damaged final .sj1 is WITHDRAWN.
//
// On-disk / framed SealJournalApplied payload (store log; little-endian).
// Full-frame store MAC covers header time fields + this payload — that store
// frame MAC is NOT a substitute for HY-SEALJRN-v1 entry_mac verification.
//   candidate_id || journal_seq || entry_mac[32] || kek_key_id ||
//   source_generation || baseline_tip_seq || baseline_tip_mac[32] ||
//   baseline_key_id || time_kind || recorded_utc_ms || embedded_type ||
//   embedded_payload_len || embedded_payload_bytes
// Applied payload field order is NOT the entry_mac domain. store_uuid is
// ambient (CURRENT store identity) at verify time — not stored in Applied.
//
// In-memory call shape (zero heap; payload owned by caller until Ack):
struct SealJournalAppliedView {
    std::uint64_t candidate_id{0};
    std::uint64_t journal_seq{0};
    std::uint8_t entry_mac[32]{};           // MUST equal recomputed HY-SEALJRN-v1
    std::uint32_t kek_key_id{0};            // selects KEK for recompute
    std::uint32_t source_generation{0};     // CompactionSourceBaseline.generation
    std::uint64_t baseline_tip_seq{0};
    std::uint8_t baseline_tip_mac[32]{};
    std::uint32_t baseline_key_id{0};
    FrameTimeKind time_kind{FrameTimeKind::ServerCorrectedUtc};
    std::int64_t recorded_utc_ms{0};        // MUST be 0 iff UnknownBootstrap
    DurableRecordType embedded_type{DurableRecordType::OrderEvent};
    std::span<const std::uint8_t> embedded_payload{};  // caller-owned; sink
                                                       // copies under Ack
    // Sink on append_seal_journal_apply(view) — NO separate FrameTimeKind arg.
    // Ambient: sink holds CURRENT store_uuid_lo/hi and the KEK table.
    // EVERY call (first insert AND idempotent Ack) MUST, in order:
    //   1. require is_seal_journal_embeddable_type(embedded_type);
    //   2. require embedded_payload.size() <= kSealJournalMaxEmbeddedBytes;
    //   3. require payload schema valid for embedded_type;
    //   4. require time_kind/recorded_utc_ms obey FrameTimeKind rules;
    //   5. require baseline 4-tuple present (caller §10.1 bind rules);
    //   6. MUST recompute expected_mac = HMAC(KEK[view.kek_key_id],
    //        HY-SEALJRN-v1 domain over ambient store_uuid + view MAC fields
    //        with record_type=embedded_type, payload_len=span.size(),
    //        payload_bytes=span); if KEK missing for kek_key_id → Corrupt;
    //      if expected_mac != view.entry_mac → Corrupt; ZERO business effects;
    //      (store frame MAC MUST NOT be used as a substitute);
    //   7. write store-frame header from view.time_kind / recorded_utc_ms;
    //   8. lookup INDEX KEY = {candidate_id, journal_seq} ONLY;
    //   9. if present: require stored entry_mac == view.entry_mac AND stored
    //      Applied authenticated fields (kek_key_id, baseline, time_*,
    //      embedded_type, embedded payload bytes) byte-equal view → Acked
    //      prior seq, NO business re-effects; any mismatch → Corrupt;
    //  10. else copy view into ONE durable framed SealJournalApplied, apply
    //      embedded business once, Ack;
    //  11. span MUST remain valid until return (no async hold).
    // Journal entry may be cleared only after that Ack.
    // Must-pass: old entry_mac + tampered payload/provenance/baseline → Corrupt,
    //            zero business side effects (first-apply and idempotent paths).
};

// DurableRecordType::SealJournalApplied is the on-disk framed record (length-
// prefixed copy of embedded_payload). Call ABI is SealJournalAppliedView ONLY
// — never a payload-less struct; never a separate FrameTimeKind parameter.

// FIXES THIS ROUND'S P1: "Acked" for every method below means the SAME thing
// it means for DurableAuditSink::append_durable() — the remote anchor
// service has DURABLY confirmed receipt (its own equivalent of an fsync'd
// write, not merely "the HTTP request returned 200" or "this client
// successfully enqueued the tuple for later delivery"). A transport-level
// success (request sent, connection acked at the TCP/HTTP layer) that
// doesn't reflect the remote service's own durability is NOT Acked under
// this contract — using it as if it were would make the entire rollback-
// protection guarantee illusory: an attacker (or ordinary process crash)
// could still lose or alter local state between "remote service returned
// 200" and "remote service actually persisted it," and this design would
// have no way to tell. Implementations of this interface MUST use whatever
// the chosen remote anchor service's own strongest available durability
// signal is (e.g. a synchronous write acknowledgment from a quorum-
// replicated store, not a fire-and-forget queue publish) — "Acked" here is
// a claim about the REMOTE side's guarantee, this client's own local
// enqueue/transport success is irrelevant to it.
```

### 10.2.1 Linearization: local append thread vs. export worker — fixes round-15 P1, and this round's 2 P0s (durable-outbox gap + ownership contradiction)

Round 15 correctly separated `append_durable()`'s LOCAL `Acked` from `ExternalAnchorClient`'s REMOTE `Acked` to keep the append path off the network — but left two real gaps this round closes: (1) the in-memory FIFO between those two steps is not itself durable, so a crash between "local append Acked" and "tuple pushed to the FIFO" silently drops that frame from ever being exported, with no recovery path noticing; (2) the prose (owner-actor pushes, export worker drains) and the `ExternalAnchorClient` interface comment ("the client maintains a bounded FIFO") named two different owners for the same queue — a genuine contradiction, not just an imprecision, since it leaves an implementer unable to tell which component actually holds the ring buffer's storage or synchronizes access to it.

**Fix — one owner, a durable-outbox recovery path, and an explicit SPSC contract:**

- **Sole FIFO owner: `DurableControlPlaneSink`/`DurableAuditSink`** — the same component that performs local appends, **not** `ExternalAnchorClient`. `ExternalAnchorClient` is a pure, stateless remote-transport client: given a tuple, it exports it and reports genuine remote-Acked or not; it holds no queue, no backlog, no retry state of its own. This resolves the ownership contradiction directly: there is exactly one FIFO, physically owned by the sink, and exactly one thread (the export worker, below) that ever touches its consumer side.
- **Fixed-capacity SPSC ring buffer, explicit memory ordering**: producer = the L5 owner actor (§9), appending locally and pushing a tuple on success; consumer = one dedicated export worker thread, single instance, never more than one. `head`/`tail` indices are each on their own cache line (`alignas(std::hardware_destructive_interference_size)`, matching `CLAUDE.md`'s false-sharing discipline) to avoid producer/consumer cache-line contention. Publication: the producer writes the new tuple's slot, then publishes by storing the advanced `tail` index with `memory_order_release`; the consumer loads `tail` with `memory_order_acquire` before reading a slot, and after fully consuming a slot (§6.1.1's dequeue-on-confirmed-remote-Ack rule below), stores the advanced `head` index with `memory_order_release`, which the producer's own capacity check loads with `memory_order_acquire`. This is the standard SPSC ring discipline — no CAS, no locking, and — critically — no ambiguity about who may write which index, closing exactly the "data race / duplicate enqueue / miscounted capacity" failure mode the review named.
- **Durable-outbox recovery, as an actual bounded procedure — fixes this round's P0 (round 16's version named the right idea but never defined a concrete, bounded drain — "re-populate the FIFO from this scan" doesn't say what happens when the scan finds more than the ring can hold, or whether new local work is admitted while recovery is still catching up)**: the in-memory ring is never assumed to be an authoritative record of "what still needs exporting" across a restart — it can't be, since it's volatile.

  1. Local `recovery_scan()`/`recover_control_plane()` must have already succeeded — `Clean`/`Recovered`, **or `ExternalAnchorUnavailable` with a valid `OperatorOverride` admission (§10.2 point 3)**. A `Corrupt`/`CapacityExceeded`/`IoError` result, or an `ExternalAnchorUnavailable` with **no** valid override, blocks this procedure exactly as it blocks everything else.
  2. **Branch on why step 1 succeeded — fixes this round's P0 (an earlier revision's unconditional "`read_latest_tip()` failure → L5 does not start" directly contradicted §10.2's sidecar override, which exists specifically to admit startup when the external anchor is unreachable; the fault-injection matrix's own "sidecar override admits" entry names exactly this scenario, and this procedure cannot silently override that with a stricter rule of its own):**
     - **Normal path (no override was needed)**: call `ExternalAnchorClient::read_latest_tip()`. If this call fails despite `recovery_scan()` reporting the anchor as reachable, that's a genuine, unexpected fault (not the already-known-unreachable case the sidecar exists for) — L5 does not start; an operator must resolve it (which may include using the sidecar override, restarting into the branch below).
     - **Degraded path (sidecar-admitted, §10.2 point 3)**: the remote tip is *known* unreachable for a live `read_latest_tip()` — that is the premise of the override — so this procedure does **not** call the remote. It does **not** skip reconstruction. Instead it reconstructs against the durable local `LastRemoteAckedTip` (§10.2.1 below) and enters the degraded-mode rules in steps 8–9. Skip only the *remote* tip fetch; do **not** treat "no remote tip" as "empty backlog."
  3. **[Normal path only]** Sequentially scan the **local durable log** (already open and verified) for frames with `(generation, sequence)` lexicographically greater than the remote tip (§10.2's ordering), in ascending order — by definition, exactly the frames locally Acked but never confirmed exported, whether because they were still in the (volatile) ring at crash time, or — the case round 16 missed — the crash landed between the local Ack and the ring push and the tuple was never enqueued at all. The *local log* still has the frame either way; that's what's scanned, never the ring.
  4. **[Normal path only]** Push tuples from the scan into the ring via `try_push()`, up to `ExportOutboxRing::kCapacity` (256) — the ring's own fixed capacity is the natural bound here, not a separate limit to re-derive. Each tuple's `enqueued_utc_ms` / age inputs come from the scanned frame's header: only `time_kind == ServerCorrectedUtc` may supply an age timestamp; `UnknownBootstrap` frames set `enqueued_utc_ms = 0` and are **excluded from age comparisons** (they still consume a ring slot and count toward frame-count lag — L5 §6.1.1).
  5. **[Normal path only]** **If the scan finds more tuples beyond the tip than fit in the ring, or the oldest reconstructed tuple with `ServerCorrectedUtc` already exceeds `kExternalAnchorHardLagMs`** (never use restart-time "now" as the enqueue time of an old frame; never treat `UnknownBootstrap` as a fresh age): this is the hard-lag condition, evaluated at startup exactly as it would be live. New local appends are refused until an operator resolves it — but the export worker still starts (step 6), since draining the backlog is what resolves the condition, and blocking the one thing that drains it would be self-defeating.
  6. **[Normal path only]** Start the export worker — it drains the ring exactly as in normal operation (Ack-gated dequeue, above). As entries are dequeued and ring capacity frees up, the **same scan cursor from step 3, held open rather than discarded** feeds the next not-yet-queued backlog frame(s) into the newly-freed slot(s), continuing until the scan reaches the local tip. Every successful remote Ack **durably updates `LastRemoteAckedTip`** in the breadcrumb directory (atomic replace + parent fsync, same contract as §10.3) **before** `pop_after_remote_ack()` advances the ring — so a crash cannot forget a tip the remote already confirmed.
  7. **[Normal path only]** New local appends are refused for the entire duration of steps 2–6 (the recovery-drain window) — admitting new work while still catching up on a *known* backlog would let the local tip run further ahead before the ring has even finished draining what was already known about, defeating the bound the whole procedure exists to enforce. Once the step-3 scan reaches the local tip, new local appends resume, subject to the ordinary live-process soft/hard-lag checks (above) — now measuring from a caught-up baseline, not a reconstructed one.
  8. **[Degraded path only] Inherited-baseline startup — fixes round-19 P0 (revision 18's empty-ring / fresh-256 baseline was a hard-lag bypass under crash→restart→new-override).** No *remote* tip is readable, but a durable **`LastRemoteAckedTip`** (breadcrumb, outside the store root, MAC under KEK, updated on every remote Ack in step 6 / live path) **is** the reconstruction baseline. Procedure:
     1. Load `LastRemoteAckedTip`. **Missing tip file + non-empty local log → `IoError`** (indistinguishable from deleting the tip to launder hard-lag — same fail-closed class as a missing chain-tip anchor). Empty log + missing tip → zeros (never exported). The admitting `OperatorOverridePayload` MUST bind the same `{generation, seq, mac}` — a mismatch refuses admission. Issuing a new override with zeros while a non-zero tip file exists is **forbidden** (operator tool must copy the on-disk tip).
     2. Scan the local log for frames with `(generation, sequence) > LastRemoteAckedTip`, exactly as step 3 would against a remote tip. Fill the ring up to capacity (step 4 rules). Hold the scan cursor open (step 6 drain/refill).
     3. Evaluate hard-lag against this **inherited** backlog (frame count beyond tip, and oldest `ServerCorrectedUtc` age). **Do not** reset the 256-frame limit as a "fresh process baseline."
     4. **Admit mode**:
        - If inherited backlog is already at/over hard-lag, OR `OperatorOverridePayload.admit_mode == ReadOnlyDrain`: start export worker + degraded alarm, but **refuse all new local appends** until the remote becomes reachable and steps 3–7 complete mid-flight (step 9). Override exists to keep recovery/drain alive, not to mint unbounded un-exported history.
        - Else (`AppendAllowedIfUnderHardLag` and under hard-lag): new appends are permitted subject to the *same* live soft/hard-lag checks, measured against the inherited oldest unexported frame / total unexported count (ring size + remaining scan cursor), never against "frames written since this process started."
     5. Raise the persistent degraded-mode alarm (distinct from soft-lag).
  9. **[Degraded path only] Reconciliation once the remote becomes reachable again**: a background task periodically retries `read_latest_tip()`. The first success: verify remote tip is equal-or-forward vs `LastRemoteAckedTip` under `(generation, sequence)` ordering (remote behind local last-acked without explanation → `Corrupt`); then run steps 3–7 against the **remote** tip mid-flight; clear the degraded alarm only when that catch-up completes; only then may ReadOnlyDrain promote to normal append admission.
  10. Any failure during the normal path's steps 2–7 keeps the writer fenced. On the degraded path, reconstruction/scan failures likewise fence appends (ReadOnlyDrain or refuse start) — the override does not authorize "proceed with an unreadable local log."
  - Normal-path crash-safety still derives from the local log + remote tip. Degraded-path crash-safety additionally requires the durable `LastRemoteAckedTip` so hard-lag cannot be laundered by restart. The override remains an availability trade for *remote* unavailability, not a license to forget local export debt.
- **Dequeue is Ack-gated, not pop-then-try**: the export worker pops a tuple only in the sense of reading it from `head`; it does **not** advance `head` until `export_tip_and_wait_bounded()` for that exact tuple returns genuinely remote-Acked **and** `LastRemoteAckedTip` has been durably updated for that tuple. A failed or timed-out export leaves the tuple in place at `head`.
- **Backpressure is still on the APPEND path**, checked before accepting a new local write: crossing `kExternalAnchorSoftLagFrames=64`/`kExternalAnchorSoftLagMs=5000` (oldest un-exported **ServerCorrectedUtc** tuple's enqueue age; UnknownBootstrap frames count toward frame lag only) raises an alarm but still admits the append; crossing `kExternalAnchorHardLagFrames=256`/`kExternalAnchorHardLagMs=30000` refuses the new local append (`append_durable()` returns `Failed`, fences the writer). Frame-count lag includes the open scan cursor's remaining unexported frames, not merely `ExportOutboxRing::size()`.
- **`LastRemoteAckedTip` monotonicity (round-21 P1):** both the export worker (per-frame Ack) and compaction (seal Ack) write the tip breadcrumb. A late old-generation Ack MUST NOT regress a tip already advanced to a newer `(generation, sequence)`.
  - **Single writer preferred:** pause/drain the export worker (or serialize tip updates onto the owner actor) before compaction begins seal export; only one tip-writer thread during seal.
  - **Or monotonic CAS:** every tip persist loads the on-disk tip, compares `(generation, sequence)` lexicographically, and writes only if the candidate is strictly forward; equal tip is idempotent Ack; regression → reject, fence, alarm — never overwrite forward with backward.
  - Compaction seal tip update uses the same rule; if CAS rejects because an unexpected forward tip already exists, treat as `Corrupt`/operator incident (equivocation), not silent ignore.
- **Compaction's `export_and_wait_ack()` (§10.2 point 4)** remains the one place this design deliberately blocks synchronously on remote durability — a rare, chunked/yielded operation, not the per-append hot path. **Before `SealExportStarted` (round-38/40):** (1) **pause tip-export producer** (`try_push` stopped); **drain `ExportOutboxRing` to empty** — export every remaining tuple to remote Ack + durable `LastRemoteAckedTip` update + `pop_after_remote_ack`, and finish every in-flight tip-export RPC (or hold the single tip-writer lock) so no stale gen-N tuple remains in the ring or on the wire; (2) complete §10.1 PreSeal network quiesce + mandatory-completion drain + journal reserve. **Only then** fsync `SealExportStarted` and call `export_and_wait_ack` as a **direct** `ExternalAnchorClient` invocation on the owner (or owner-driven poll loop) — **never** by enqueueing the seal onto the paused tip-export worker ring. On seal success it MUST update `LastRemoteAckedTip` to the seal's final tip **via the monotonic rule above**, **before** `CURRENT` flips. **After tip@N+1:** discard any residual gen-N ring tuples **without sending** (drain should have emptied the ring; residual is a fence/`Corrupt` signal if non-empty under a compliant PreSeal). Remote tip endpoints **must hard-reject** lexicographically lagging `(generation, sequence)` (soft remote that accepts regression is non-compliant). During the seal wait the owner must still service the PostSeal journal handoff SPSC (§10.1) — a pure blocking sleep that ignores mandatory completions is illegal. On authenticated-`NotFound` abandon / ClrAbandoned, run GenGone + TipExportProducerResume (§10 ABI / §10.1) before admitting new tip exports or a new PreSeal candidate. "Every remote Ack updates the tip" is not limited to the SPSC export worker.

```cpp
// Durable export baseline — breadcrumb directory, outside store root.
// Updated on every remote Ack BEFORE ring pop, under monotonic
// (generation, sequence) CAS / single-writer (§10.2.1). Survives
// crash/restart; degraded-mode reconstruction baseline when remote down.
struct LastRemoteAckedTip {
    std::uint64_t store_uuid_lo{0};
    std::uint64_t store_uuid_hi{0};
    std::uint32_t generation{0};
    std::uint64_t sequence{0};
    std::array<std::uint8_t, 32> tip_mac{};
    std::uint32_t key_id{0};
    std::uint8_t mac[32]{};  // HMAC under KEK over the above
};

// Pseudocode — tip persist (export worker OR compaction seal path):
//   auto cur = load_tip();
//   if (candidate < cur) { fence("tip regression"); return Failed; }
//   if (candidate.generation == cur.generation &&
//       candidate.sequence == cur.sequence) {
//     if (tip_mac/key_id/store_uuid mismatch) return Corrupt; // equivocation
//     return Acked;  // idempotent
//   }
//   return durable_replace_tip(candidate);  // strictly forward; atomic+fsync
```

```cpp
// hengyuan/durable_control_plane.hpp — NEW, fixes this round's P1: the
// struct ExportOutboxRing/ExternalAnchorClient were already written against,
// but never itself defined. Fixed-size, trivially copyable, no heap.
struct ExportTuple {
    std::uint64_t store_uuid_lo{0};
    std::uint64_t store_uuid_hi{0};
    std::uint32_t generation{0};
    std::uint64_t sequence{0};
    std::array<std::uint8_t, 32> tip_mac{};
    std::uint32_t key_id{0};
    std::int64_t enqueued_utc_ms{0};  // ServerCorrectedUtc only; 0 when
                                        // frame time_kind is UnknownBootstrap
                                        // (must not participate in age lag).
    std::uint8_t time_kind{0};        // FrameTimeKind; copied from header
};

// hengyuan/durable_control_plane.hpp — the FIFO lives HERE, owned by the
// sink implementation, never inside ExternalAnchorClient.
class ExportOutboxRing {
public:
    static constexpr std::size_t kCapacity = 256;  // = kExternalAnchorHardLagFrames

    // Producer-side (owner actor thread ONLY). Returns false if the ring is
    // at capacity — the caller (append path) treats this as the hard-lag
    // fence, per above.
    bool try_push(const ExportTuple& t) noexcept;

    // Consumer-side (export worker thread ONLY). Returns false if empty.
    // Does NOT remove the entry — see pop_after_remote_ack().
    bool peek_oldest(ExportTuple& out) const noexcept;
    // Consumer-side ONLY, called after export_tip_and_wait_bounded() for
    // the peeked tuple returns genuinely Acked AND LastRemoteAckedTip has
    // been durably updated for that tuple. Advances head.
    void pop_after_remote_ack() noexcept;

    std::size_t size() const noexcept;  // acquire-loads both indices

private:
    struct alignas(std::hardware_destructive_interference_size) Head {
        std::atomic<std::size_t> value{0};
    } head_;
    struct alignas(std::hardware_destructive_interference_size) Tail {
        std::atomic<std::size_t> value{0};
    } tail_;
    std::array<ExportTuple, kCapacity> slots_{};  // fixed, no heap
};

// Pure remote-transport client — fixes this round's P0 (ownership
// contradiction): holds NO queue, NO backlog, NO retry state. Every
// enqueue/dequeue/backpressure decision belongs to ExportOutboxRing above,
// owned by the sink — never here.
class ExternalAnchorClient {
public:
    virtual ~ExternalAnchorClient() = default;
    // Called by the export worker for exactly the tuple ExportOutboxRing::
    // peek_oldest() currently returns — one outstanding export at a time
    // (single consumer, single in-flight call). This method has no notion
    // of "too far behind" and does no queueing of its own; that's the
    // ring's job (§10.2.1).
    // Round-40: remote MUST hard-reject any tip that is lexicographically
    // lagging vs the remote's last accepted `(generation, sequence)` for this
    // store_uuid (soft-accept / silent overwrite of a newer tip is forbidden).
    // Local client maps that reject to Failed (not Acked); the tuple stays at
    // ring head until policy resolves — after a seal tip@N+1, PreSeal drain
    // guarantees no gen-N tuple remains to send (§10.1 / §10.2.1).
    virtual AuditAppendResult export_tip_and_wait_bounded(
        std::uint64_t store_uuid_lo, std::uint64_t store_uuid_hi,
        std::uint32_t generation, std::uint64_t sequence,
        std::span<const std::uint8_t, 32> tip_mac, std::uint32_t key_id) noexcept = 0;
    // Round-37: seal MUST already have a durable SealExportStarted whose
    // request_id == seal.request_id BEFORE the first network write of this call.
    // Implementations MUST NOT send seal bytes if that breadcrumb is missing.
    virtual AuditAppendResult export_and_wait_ack(const GenerationSeal&) noexcept = 0;
    // Authenticated, monotonic read. Unavailable is distinct from malformed,
    // equivocal or stale data; only the latter three are Corrupt. Used both
    // for §10.2's rollback check and §10.2.1's durable-outbox reconstruction
    // at startup.
    virtual RecoveryScanStatus read_latest(GenerationSeal& out) noexcept = 0;
    virtual RecoveryScanStatus read_latest_tip(
        std::uint64_t store_uuid_lo, std::uint64_t store_uuid_hi,
        std::uint32_t& out_generation, std::uint64_t& out_sequence,
        std::array<std::uint8_t, 32>& out_tip_mac, std::uint32_t& out_key_id) noexcept = 0;
    // Round-37/39 — recovery query by SealExportStarted.request_id.
    // Returns SealQueryStatus — NEVER RecoveryScanStatus (no Found/NotFound there).
    // Found: out filled; caller MUST field-bind to SealExportStarted + local N+1
    //   including Started baseline 4-tuple == bridge.prev_* (full, incl.
    //   prev_generation / prev_key_id) — see §10.1 recovery step 0.
    // NotFound: authenticated permanent-negative only → may abandon IFF N tip
    //   still equals Started/bridge baseline 4-tuple; else Corrupt (§10.1).
    //   Abandon ONLY via ClrAbandoned (`.abd` A0 first) whether C present or not
    //   (§10 ABI round-59…63) — never clear Started without durable A; never leave
    //   unauthorized C occupying the name.
    // TransportUnavailable: keep SealExportStarted; fence; re-query; never `.abd`.
    // Corrupt: MAC/equivocation/bind failure.
    virtual SealQueryStatus query_seal_by_request_id(
        std::uint64_t store_uuid_lo, std::uint64_t store_uuid_hi,
        std::uint64_t request_id, GenerationSeal& out) noexcept = 0;
};

class DurableControlPlaneSink {
public:
    virtual ~DurableControlPlaneSink() = default;
    virtual AuditAppendResult append_rate_freeze(
        const RateLimitFreezePayload& freeze,
        FrameTimeKind time_kind) noexcept = 0;
    // Round-31/32: sink MUST reject append_rate_freeze if
    //   freeze.conservative_wait_ms > 0 &&  // THIS frame's contribution
    //   (freeze.wait_generation == 0  // wait-bearing frames must be explicit
    //    || prior folded wait_generation == UINT32_MAX  // overflow fence
    //    || freeze.wait_generation != prior_folded_wait_generation + 1);
    // and if freeze.conservative_wait_ms == 0 &&
    //   freeze.wait_generation != prior_folded_wait_generation (must copy-forward).
    // New episode (no prior): wait_generation must be 1 if wait>0, else 0.
    // Legacy-wait migration (prior max==0, wait>0): first post-upgrade
    // wait-bearing frame MUST be wait_generation==1 (same rule as new episode
    // step from prior 0).
    // On Ack of a frame that advances wait_generation: any in-process
    // arm_ack_steady for a prior generation is immediately invalid (must re-Arm).
    // LIVE OWNER ONLY — compaction MUST NOT call this for retained aggregates.

    // Round-33/34 P0 — compaction-only folded freeze into non-CURRENT gen-N+1.
    // Writes DurableRecordType::RateLimitFreezeSnapshot.
    // Sink MUST:
    //  - reject unless compaction session is active for target generation
    //    == CURRENT+1 (writing only into gen-N+1/ while CURRENT still names N);
    //  - reject if source_generation == UINT32_MAX (no target+1) — fence +
    //    offline store migration required (round-34 P1);
    //  - reject if proof.target_generation != source_generation + 1;
    //  - reject if proof.source_* tip does not equal the session's
    //    CompactionSourceBaseline (and that baseline must still match live
    //    CURRENT tip — caller re-verifies after yields; sink may re-read);
    //  - reject if proof.source_generation / tip MAC / folded fields do not
    //    match the compaction retain scan (bind fold evidence);
    //  - accept wait_generation == proof.folded_wait_generation (including
    //    values > 1 and legacy-seal 1) — NO prior_folded+1 check;
    //  - treat payload.conservative_wait_ms / deadline_utc_ms / wait_generation
    //    as FOLDED snapshot values (not live event contributions);
    //  - NOT touch arm_ack_steady / reserved probe state;
    //  - reject if invoked by the live owner actor outside compaction.
    struct CompactionFreezeSnapshotProof {
        std::uint32_t source_generation{0};
        std::uint64_t source_tip_seq{0};        // CompactionSourceBaseline tip
        std::uint8_t source_tip_mac[32]{};
        std::uint32_t source_key_id{0};
        std::uint32_t target_generation{0};     // MUST be source+1; refuse at MAX
        std::uint32_t freeze_epoch{0};
        std::int64_t folded_deadline_utc_ms{0};
        std::int64_t folded_conservative_wait_ms{0};
        std::uint32_t folded_wait_generation{0};
        std::uint8_t folded_source{0};
    };
    virtual AuditAppendResult append_compacted_freeze_snapshot(
        const RateLimitFreezePayload& folded_snapshot,
        FrameTimeKind time_kind,
        const CompactionFreezeSnapshotProof& proof) noexcept = 0;

    // Round-33/34/36 — compaction-only SINGLE frame CompactedFreezeWaitEvidence.
    // Payload is CompactedFreezeWaitEvidencePayload ONLY — never a pair of
    // live Arm/Satisfy record types. Sink MUST:
    //  - same compaction-session / non-CURRENT / baseline-tip gates as snapshot;
    //  - reject source_generation == UINT32_MAX / target != source+1;
    //  - require evidence.wait_generation > 0 and == folded snapshot gen;
    //  - require satisfy_arm_frame_seq == source_arm_seq;
    //  - require evidence.source_baseline_{tip_seq,tip_mac,key_id} ==
    //    CompactionSourceBaseline / proof.source_* (round-36 key_id on frame);
    //  - for legacy path-3: require source_arm_seq and source_satisfy_seq both
    //    strictly > every wait-bearing live RateLimitFreeze in source epoch
    //    at baseline scan time, bound matches folded wait;
    //  - for ordinary retain: require pair was current-gen evidence in scan;
    //  - NOT create/update arm_ack_steady; NOT re-validate steady elapsed;
    //  - reject live-owner calls outside compaction.
    struct CompactionWaitEvidenceProof {
        std::uint32_t source_generation{0};
        std::uint64_t source_tip_seq{0};
        std::uint8_t source_tip_mac[32]{};
        std::uint32_t source_key_id{0};
        std::uint32_t target_generation{0};
        std::uint32_t freeze_epoch{0};
        std::uint32_t wait_generation{0};       // explicit; > 0
        bool legacy_sequence_proven_rewrite{false};
    };
    virtual AuditAppendResult append_compacted_wait_evidence(
        const CompactedFreezeWaitEvidencePayload& evidence,
        FrameTimeKind time_kind,
        const CompactionWaitEvidenceProof& proof) noexcept = 0;

    virtual AuditAppendResult append_snapshot(
        const SymbolRegistrySnapshotPayload& snap,
        std::span<const SymbolRules> entries) noexcept = 0;
    virtual AuditAppendResult append_weight_config(
        const EndpointWeightConfig& cfg) noexcept = 0;
    virtual AuditAppendResult append_usage_snapshot(
        const RateLimitUsageSnapshotPayload& usage) noexcept = 0;
    // Appends GenerationBridge into the *new* generation before CURRENT flips
    // (§10.2 / L5 §6.1.3). Not usable as a general post-refuse recovery write.
    virtual AuditAppendResult append_generation_bridge(
        const GenerationBridgePayload& bridge) noexcept = 0;
    // Post-admission audit mirror ONLY — never the admission evidence itself
    // (that lives in OperatorOverrideSidecar). Called once the sink is open.
    virtual AuditAppendResult append_operator_override(
        const OperatorOverridePayload& ov) noexcept = 0;
    // NEW — fixes this round's P0: §7.3.1's FreezeProbeCredit claimed a
    // durable, recoverable attempt count backed by this write, but no
    // DurableControlPlaneSink method for it ever existed — the only append
    // path available was L5's DurableAuditSink::append_durable(), which
    // would have reintroduced the exact L4-depends-on-L5 cycle round 9
    // closed (§7.3.1's probe runs on L4-only paths too — account refresh,
    // startup baseline — that have no L5 DurableAuditSink to call at all).
    // This method is the correct, L4-owned home for it, alongside every
    // other control-plane append above.
    // Round-26/27/28 — FreezeProbeAttempt. Sink MUST:
    //  - reject attempt.cleared == true;
    //  - reject if freeze_epoch is not the single active uncleared epoch;
    //  - reject if attempt_ordinal != (max durable ordinal for that epoch) + 1
    //    (no gaps, no duplicates, no rewind);
    //  - reject if folded deadline_utc_ms > 0 and
    //    attempt.not_before_utc_ms < folded deadline_utc_ms;
    //  - reject if attempt_ordinal == 0 or > kMaxTotalAttempts;
    //  - NEW (round-30/31/32 P0): reject if folded `conservative_wait_ms > 0` and
    //    there is no already-Acked, sink-verified `FreezeWaitSatisfied` for this
    //    epoch with `bound_conservative_wait_ms == folded wait` AND
    //    `wait_generation == folded wait_generation` AND `wait_generation > 0`
    //    (stale-generation Satisfy after equal-wait rematch MUST reject —
    //    round-31; legacy gen 0/missing MUST reject — round-32);
    //    also reject while legacy-wait (folded wait>0 && folded gen==0) —
    //    migration seal required before any probe attempt;
    //    `try_reserve_probe(..., bool wait_ok)` remains a FAST-PATH hint only;
    //  - accept purpose ∈ {DeadlineOrVerify, ClockRepublishOrVerify}; both burn
    //    the same 8-cap (round-28/29 — neither is a free lane);
    //  - reject ClockRepublishOrVerify if a trustworthy clock is already
    //    published (that purpose is only for now_utc-absent re-publish+verify);
    //  - on Ack: return AuditAppendResult with .sequence set; caller MUST
    //    pass that full result into confirm_attempt_acked before any /time
    //    send (round-27 P1). sequence==0 is legal (generation genesis).
    //  - Note: FreezeClear{ProbeVerified} after ClockRepublishOrVerify is
    //    legal when proof+deadline+WaitSatisfied(current gen) hold (round-29/31)
    //    — append path is append_freeze_clear, not this method.
    virtual AuditAppendResult append_freeze_probe_attempt(
        const FreezeProbeAttemptPayload& attempt,
        FrameTimeKind time_kind) noexcept = 0;
    // Round-22/25/26/31 — terminal-clear path. Sink MUST:
    //  - reject attempt.cleared==true on append_freeze_probe_attempt;
    //  - reject ProbeVerified/ConservativeWaitCompleted if folded source==2;
    //  - ProbeVerified: verify FreezeTimeProbeProof (extended MAC domain,
    //      including proof.tls_verified_host in allowlist + MAC domain);
    //    if folded conservative_wait_ms > 0: REQUIRE an already-Acked
    //      sink-verified FreezeWaitSatisfied for this epoch with
    //      bound_conservative_wait_ms == folded wait AND
    //      wait_generation == folded wait_generation AND wait_generation > 0
    //      (required-present at clear Ack — do NOT mark "consumed" before
    //      clear succeeds; a Failed clear must leave WaitSatisfied reusable
    //      for this gen); reject clear while legacy-wait (folded gen==0);
    //      equality of FreezeClearPayload.bound_conservative_wait_ms alone
    //      is NOT sufficient; stale-generation / legacy-gen Satisfy is NOT
    //      sufficient (round-25/26/31/32);
    //    clear.bound_wait_generation MUST equal folded wait_generation when
    //      wait>0; MUST be 0 when wait==0;
    //    if conservative_wait_ms == 0: WaitSatisfied not required;
    //  - ConservativeWaitCompleted ONLY when folded deadline_utc_ms == 0;
    //    if conservative_wait_ms > 0: SAME generation-bound WaitSatisfied
    //      requirement (round-26/31 — not optional);
    //  - OperatorAuthorized: published clock + KEK MAC + tip bind + nonce;
    //  - Ack before in-memory lift / release_probe clear.
    virtual AuditAppendResult append_freeze_clear(
        const FreezeClearPayload& clear,
        FrameTimeKind time_kind) noexcept = 0;
    // Round-26/31/32 P0 — arm a conservative wait. Sink MUST:
    //  - reject if no active freeze for freeze_epoch;
    //  - reject if bound_conservative_wait_ms != folded wait;
    //  - reject if folded wait > 0 && wait_generation == 0 (gen 0 illegal
    //    whenever a wait is active — blocks Arm-before-migration on legacy-wait);
    //  - reject if wait_generation != folded wait_generation;
    //    (legacy-wait folded gen==0: Arm impossible until migration seals gen≥1);
    //  - reject if source==2;
    //  - on Ack: record process-local arm_ack_steady[epoch] = steady_now,
    //    arm_frame_seq, arm_ordinal, bound, wait_generation (OVERWRITE prior
    //    incomplete arm when generation or arm_ordinal advances);
    //  - NOT clear the episode.
    virtual AuditAppendResult append_freeze_wait_arm(
        const FreezeWaitArmPayload& arm,
        FrameTimeKind time_kind) noexcept = 0;
    // Round-25/26/31/32 P0 — non-terminal wait evidence. Sink MUST:
    //  - reject if no active freeze for freeze_epoch;
    //  - reject if bound_conservative_wait_ms != folded wait;
    //  - reject if folded wait > 0 && wait_generation == 0;
    //  - reject if wait_generation != folded wait_generation (stale gen);
    //  - reject if source==2 (permanent has no timed wait to satisfy);
    //  - reject if no live-session Arm for (epoch, arm_ordinal, wait_generation)
    //    whose arm_frame_seq matches payload.arm_frame_seq;
    //  - reject unless (steady_now_at_satisfy_ack - arm_ack_steady) >= bound
    //    AND elapsed_steady_ms_claimed >= bound;
    //  - NOT clear the episode / NOT set out_has_freeze false;
    //  - after wait_generation advances, prior WaitSatisfied is insufficient
    //    even if bound ms is equal (round-31 equal-wait rematch);
    //  - legacy gen-0 Satisfy is never acceptable as current evidence (round-32).
    virtual AuditAppendResult append_freeze_wait_satisfied(
        const FreezeWaitSatisfiedPayload& wait,
        FrameTimeKind time_kind) noexcept = 0;
    // NEW — freeze_epoch durable high-water: next allocatable epoch.
    // Ack watermark with next=E+1 BEFORE using epoch E (round-21 P0).
    // Only for NEW episodes (round-22); merges skip this.
    // Never derive by scanning (compaction may drop resolved freeze/probe
    // history). time_kind REQUIRED (round-20) — Phase-B episodes use
    // UnknownBootstrap matching the freeze that follows.
    virtual AuditAppendResult append_freeze_epoch_watermark(
        std::uint32_t next_freeze_epoch,
        FrameTimeKind time_kind) noexcept = 0;

    // Round-40…46 — exactly-once seal-journal APPLY as ONE durable frame.
    // Call shape: SealJournalAppliedView ONLY (time_* + baseline + kek_key_id).
    // FORBIDDEN: separate FrameTimeKind arg; FORBIDDEN: MAC-omitted baseline;
    // FORBIDDEN: opaque-only entry_mac compare without HY-SEALJRN-v1 recompute.
    // Ambient: CURRENT store_uuid + KEK table (same class as SealExportStarted).
    // Sink MUST on EVERY call (see SealJournalAppliedView comment):
    //  - allowlist + size + schema + FrameTimeKind rules;
    //  - MUST recompute HY-SEALJRN-v1 over ambient store_uuid + view fields;
    //    view.entry_mac != expected → Corrupt; zero business effects;
    //  - index {candidate_id, journal_seq} ONLY; present → byte-equal stored
    //    Applied authenticated fields + same entry_mac → idempotent Ack;
    //    else Corrupt; absent → one framed apply + Ack;
    //  - store frame MAC is NOT a substitute for entry_mac recompute;
    //  - refuse naked append_* of embedded payload as journal apply;
    //  - span valid until return.
    // Caller / recovery: §10.1 baseline + wire MAC-valid before call.
    // Compaction: while any seal-journal entry for candidate_id remains,
    // retain every SealJournalApplied for that candidate (or refuse
    // compaction). Retained Applied MUST keep baseline 4-tuple + kek_key_id
    // + entry_mac intact until the frame is dropped entirely.
    virtual AuditAppendResult append_seal_journal_apply(
        const SealJournalAppliedView& apply) noexcept = 0;

    // Recovery (round-22/25/26/27/31/32):
    //  - out_has_freeze / out_active_freeze = folded active state (§7.3.1)
    //    including folded wait_generation (raw max; 0 means legacy/unspecified
    //    when wait>0 — see legacy-wait migration, do NOT synthesize match)
    //  - out_permanent_latch / out_uncleared_epoch_count as before
    //  - out_has_wait_satisfied = true ONLY if FreezeWaitSatisfied OR
    //    CompactedFreezeWaitEvidence (single CompactedFreezeWaitEvidencePayload)
    //    matches binding epoch AND wait_generation == folded wait_generation
    //    AND wait_generation > 0 (legacy gen 0/missing → always false)
    //  - CompactedFreezeWaitEvidence counts as Satisfy for recovery gates
    //    but does NOT restore arm_ack_steady (if compacted Satisfy present for
    //    current gen, skip re-wait — same as live Satisfy Acked in prior process)
    //  - legacy-wait (wait>0 && folded gen==0): out_has_wait_satisfied=false;
    //    caller MUST migration/compaction-seal before probe/clear (§7.3.1)
    //  - out_has_wait_arm = latest FreezeWaitArm for binding epoch+generation
    //    WITHOUT a subsequent matching WaitSatisfied — informational only;
    //    live process MUST re-Arm if no current-gen Satisfy (prior arm_ack_steady
    //    is gone across restart); legacy Arm never counts as live-session Arm
    //  - After return, caller MUST
    //      credit.restore_from_recovery(
    //          out_has_freeze ? out_active_freeze.freeze_epoch : 0,
    //          {out_freeze_probe_attempts.data(),
    //           out_freeze_probe_attempt_count},
    //          out_has_freeze ? out_active_freeze.deadline_utc_ms : 0,
    //          steady_now, now_utc_ms_or_nullopt);
    //    before any try_reserve_probe (round-25/27). folded deadline is
    //    REQUIRED so multi-day UTC bans survive crash without short-backoff
    //    probe storms.
    //  - After a NEW episode's watermark+freeze Ack:
    //      credit.bind_new_episode_after_durable_create(
    //          epoch, payload.deadline_utc_ms);
    //  - After legacy-wait migration freeze Ack (round-32): treat like a
    //    wait-generation advance — no Satisfy yet; Arm under gen 1.
    virtual RecoveryScanStatus recover_control_plane(
        RateLimitFreezePayload& out_active_freeze,
        bool& out_has_freeze,
        bool& out_permanent_latch,
        std::uint8_t& out_uncleared_epoch_count,
        EndpointWeightConfig& out_weights,
        bool& out_has_weights,
        RateLimitUsageSnapshotPayload& out_usage,
        bool& out_has_usage,
        GenerationBridgePayload& out_bridge,
        bool& out_has_bridge,
        std::uint32_t& out_next_freeze_epoch,
        bool& out_has_freeze_epoch_watermark,
        std::array<FreezeProbeAttemptPayload, 8>& out_freeze_probe_attempts,
        std::size_t& out_freeze_probe_attempt_count,
        FreezeClearPayload& out_latest_clear,
        bool& out_has_clear,
        FreezeWaitSatisfiedPayload& out_wait_satisfied,
        bool& out_has_wait_satisfied,
        FreezeWaitArmPayload& out_wait_arm,       // NEW round-26
        bool& out_has_wait_arm) noexcept = 0;     // NEW round-26
};

// Sidecar-only API (same header, separate class) — round-12 P0.
// Lives beside the breadcrumb path, OUTSIDE the store root. Does not require
// the hash-chained log to be open for append. Offline operator tool + recovery
// admission both use this; the running fenced L5 process does not.
class OperatorOverrideSidecar {
public:
    virtual ~OperatorOverrideSidecar() = default;
    // Creates final nonce-named evidence with OS no-replace semantics
    // (POSIX linkat/renameat2(RENAME_NOREPLACE); Windows CREATE_NEW). A
    // replace-capable rename is forbidden. Canonical bytes + file and parent
    // durability barriers are required; an existing nonce is a hard failure.
    virtual AuditAppendResult write_override(
        const OperatorOverridePayload& ov) noexcept = 0;
    virtual bool read_override(OperatorOverridePayload& out) const noexcept = 0;
    // Atomically creates an immutable nonce-named consumed tombstone outside
    // the store (CREATE_NEW/no-replace, MAC-protected, fsynced). It is never
    // deleted by compaction. Recovery rejects a nonce with any tombstone.
    virtual AuditAppendResult consume_after_successful_admission(std::uint64_t nonce) noexcept = 0;
};
```

Freshness/TTL is measured **only** from `steady_at_fetch_ms` to the caller's current `steady_clock` reading; publication queue delay never extends a snapshot's usable life. All stored clock values are signed milliseconds; subtraction/addition uses checked arithmetic and a negative elapsed value or overflow makes the snapshot stale/fail-closed.

**Ownership:** only the owner actor thread (§9) calls live `append_*`. Compaction (same owner thread, compaction session) alone may call `append_compacted_freeze_snapshot` / `append_compacted_wait_evidence` into `gen-N+1` before `CURRENT` flips, and only while `CompactionSourceBaseline` still matches live CURRENT tip (§10.1). L5's `DurableAuditSink` is a **subtype / same backend** that additionally exposes L5-owned order-event/checkpoint appends and order-aware `recovery_scan()`; it must not be a second independent writer to a different file. Compaction retention for freezes (L5 §6.1.3, round-25/26/31/32/33/34): fold all frames per uncleared epoch; emit folded freeze only via compaction snapshot ABI; retain matching wait evidence **for current explicit `wait_generation > 0` only** via single-frame `CompactedFreezeWaitEvidencePayload`; legacy-wait seal emits snapshot gen `1` and drops unproven Satisfy unless Arm+Satisfy both seq-after last wait-bearing; tip-mismatch after yield aborts candidate; never drop uncleared `source=2`; valid proven terminal `FreezeClear` may drop resolved history; watermark always retained. Permanent clear only via `append_freeze_clear(OperatorAuthorized)`. After recovery, `FreezeProbeCredit::restore_from_recovery(..., folded_deadline_utc_ms, ...)` is mandatory before probes; new episodes call `bind_new_episode_after_durable_create(epoch, deadline)` only after watermark+freeze Ack; legacy-wait episodes migration-seal before probe/clear.

**L5 dependency direction (normative):** L5 may reference this section; this section must never require an L5 type to compile or to specify behavior.

### 10.1 Store size bounds — fixes round-10/12 P1

- `kMaxLogBytes = 256 MiB` and `kMaxLogFrames = 4,000,000` per generation — hard caps, checked before every append.
- **High-watermark compaction**: crossing 75% of either cap enqueues compaction (L5 §6.1.3) at control-plane priority. **Chunk/yield (round-12 P1; tip-pin round-34 P0)**: compaction runs as a state machine of bounded steps (`kCompactionChunkFrames = 4096` frames or `kCompactionChunkMs = 50` of steady work per actor turn), then yields so kill-switch / clock refresh / freeze handling can run. Compaction must never be a single blocking loop on the owner actor.
  - **CompactionSourceBaseline (round-34 P0):** at retain-scan start, pin `{generation=N, tip_seq, tip_mac, key_id}` of live CURRENT. All `CompactionFreezeSnapshotProof` / `CompactionWaitEvidenceProof` / `GenerationBridge.prev_tip_*` / `prev_key_id` bind this baseline.
  - **CompactionCandidateIntent before G (round-64/65/66/67/71/72):** after pin baseline and **before** creating any `gen-N+1/` directory/byte, `CREATE_NEW`+fsync packed **`CompactionCandidateIntentWire`** (140B; phase=**Building**; baseline 4-tuple + `target_generation=N+1` + fresh `build_nonce`; `candidate_id=request_id=0`; MAC under `kek_key_id`; **genesis** — no none→Building `.x1`). Refuse if a prior Intent or orphan `.x1` / leftover `.xgc` still exists (finish that candidate’s cleanup first — dual/leftover Intent incl. **PostSealFinalizing** / **AbandonFinalizing** / residual transitions / residual GC auth is a hard fence, never overwrite). Only then create/write/fsync G. Every later phase raise: durable **`CompactionIntentTransitionWire`** `.x1` (176B; `HY-COMPINTENT-X-v1`; no-replace) **then** REPLACE Intent — id-reserve → `.x1` seq=1 Building→**Reserved**; Started publish → `.x1` seq=2 → **StartedPublished**; `.clr` Authorized → `.x1` seq=3 → **PostSealFinalizing** before any Started unlink; ResumeAuthorized → `.x1` seq=3 → **AbandonFinalizing** while A present, before unlink A. Clear (r67/r70/r71): **CAPTURE DurableCleanupAuthEvidence → unlink C/A → CREATE `.xgc`** (`CompactionIntentGcAuthorizedWire` **316B**; `format_version=2`; `HY-COMPINTENT-GC-v2`; no-replace; embeds DurableCleanupAuthEvidence) → **TipExportProducerResume (idempotent)** → GC matching `.x1` → clear Intent → unlink `.xgc` last — only at final cleanup (§10 ABI). Legacy v1 204B / `HY-COMPINTENT-GC-v1` → **fail-closed** on recovery (never the prescribed write).
  - **After every yield resume**, and **immediately before** seal export, tip-persist, and CURRENT flip: re-read CURRENT tip; if `{generation, tip_seq, tip_mac, key_id} != baseline` → **PreSeal-abandon** the `gen-N+1/` candidate (do not seal/flip): verify vs Intent if present, discard/relocate partial N+1 under pre-Started GenGone rules, TipExportProducerResume if pause armed, **CREATE `.xgc` (316B; `HY-COMPINTENT-GC-v2`; PreSealAbandonClear; evidence zeros) → GC `.x1` → clear Intent → unlink `.xgc` last**, and **restart** compaction from a fresh scan (baseline now includes the new N tail — e.g. a 429 that advanced `wait_generation`). Events on N are never dropped. Tip≠baseline during Building is **never** Corrupt solely for residual G.
  - Live appends to N during the build/yield phase are legal (same owner thread handling freeze) and are exactly what tip-mismatch detects.
  - **Seal quiesce — mandatory durable inputs (round-35/36/37 P0; withdraws volatile replay buffer):** from the successful final tip-check through CURRENT flip, the owner must not yield for unrelated work. **Mandatory durable inputs** are observations that must survive power-loss before the caller may treat them as handled, including at least: `RateLimitFreeze` / freeze wait Arm·Satisfy / `FreezeClear` from a received 429/418/ban; exchange-observed order outcomes (`OrderSubmitted`/`Accepted`/`PartialFill`/`Filled`/`Cancelled`/`Rejected`/reconcile confirms) once the response bytes are accepted as authoritative. For these:
    - **Volatile in-memory replay buffers are forbidden.** Returning `Acked` / “handled” to the caller before a durable write is a defect.
    - Any caller-visible success / “handled” for a **received** 429/418 or exchange order outcome **requires** a durable A-append Ack **or** a durable B-journal fsync. Returning “retryable busy”, non-durable `Failed`, or deferring **without** that durable write (and without retaining the observation bytes under a hard fence for mandatory retry) is **illegal** — crash would drop the ban/fill.
    - **Phase split (round-36/37):**
      - **PreSeal** = after successful final tip-check, **before** a durable `SealExportStarted` exists for this candidate. Path **A or B** legal.
      - **PostSeal** = from the moment `SealExportStarted` is fsynced (whether or not local seal Ack was observed) through tip persist through CURRENT flip + journal drain. Path **B only**; path A illegal. Tip must remain == baseline; tip advance / path-A → fence + `Corrupt`.
      - Deep trap (round-37 P0): defining PostSeal only by “entered `export_and_wait_ack` in this process” fails when remote already persisted the seal but local power-loss prevented Ack — recovery would look PreSeal and could journal-replay onto N. **`SealExportStarted` is the sole local PostSeal gate.**
    - **PreSeal admission before `SealExportStarted` (round-37…56 — journal-full deadlock + outbox regression + id-before-B + journal↔apply closed loop + B-sticky + journal continuity + no-replace/tombstone + drain-complete + Windows IO + linearizable intake-close + registered mask/deadline + post-close catch-up + durable StartedWire v2 + §10.3 single-chain + companion migrate):**
      1. **Tip-export serialize + full outbox drain (round-38/40):** pause tip-export producer (`try_push` stopped). Drain **in-flight tip-export Acks** / hold tip-writer lock **and** drain `ExportOutboxRing` until `size()==0` with every prior tuple remote-Acked + `LastRemoteAckedTip` durably updated + popped (§10.2.1) — **before** any seal breadcrumb. Leaving backlog tuples in the 256-slot ring after seal tip@N+1 is **illegal**. Producer remains paused while any PostSeal/abandon gate is live (`SealExportStarted`, CleanupInProgress, ClrUnauthorized, AbandonInProgress, Intent *Finalizing*). **TipExportProducerResume** (round-61…72): (i) after proof-valid `.clr` Authorized → **`.x1`→Intent PostSealFinalizing** → ordered unlink M→V→L → **CAPTURE from C → unlink C → CREATE `.xgc` (316B; `HY-COMPINTENT-GC-v2`; DurableCleanupAuthEvidence)** → TipExportProducerResume `try_push` for gen-N+1 (**idempotent**) → **GC `.x1` → clear Intent → unlink `.xgc` last**; (ii) after ClrAbandoned **ResumeAuthorized** (GenGone done; CURRENT/tip re-verified against A while `.abd` present; producer still paused) → **`.x1`→Intent AbandonFinalizing** (A still present) → **CAPTURE from A → unlink A → CREATE `.xgc` (316B evidence)** → TipExportProducerResume for gen-N (**idempotent**) → **GC `.x1` → clear Intent → unlink `.xgc` last** (never re-read unlinked A; never resume-before-unlink / never TipExport-before-CREATE) — covers authenticated-NotFound Started abandon **with or without C** (unified A0…A4; no-C uses present_mask.C=0 + digest_C=0); (iii) after Path-A / tip-mismatch PreSeal-abandon that ran after step-1 pause (**Intent Building|Reserved**; **no Started**; does **not** require `.abd`; G absent without T legal) → discard/relocate partial N+1 → TipExportProducerResume if pause armed → **CREATE `.xgc` (316B; PreSealAbandonClear; evidence zeros)** → **GC `.x1` → clear Intent → unlink `.xgc` last** → resume for gen-N already done or idempotent. Forbidden: resume that requires fields from an already-unlinked `.abd`; Forbidden: ClrAbandoned resume-before-unlink; Forbidden: successful abandon that leaves producer paused (hard-lag fence); Forbidden: NotFound-without-C side-path that clears Started without `.abd`; Forbidden: clear Intent before GenGone / before A unlink on abandon paths; Forbidden: clear Intent / unlink last Started before PostSealFinalizing; Forbidden: unlink A before AbandonFinalizing.
      2. **Network quiesce:** stop issuing new private REST requests that can produce mandatory durable outcomes; refuse new non-mandatory submits with retryable busy.
      3. **Durable id reservation BEFORE any Path B (round-42/47/51/55/56/57/58/64/65):** **Refuse** new `SealIdWatermark` reserve / new PreSeal candidate while **any** prior `candidate_id` has incomplete drain — live `.sj1`, unapplied work, residual `.jhw` / `.jts`, a complete **NativeV2Started** / **MigratedV2Started**, a `LegacyStarted` (192B), any migration artifact (`.v2` / `.mig`), a cleanup/abandon tombstone (`.clr` / `.abd`), **or** a live **`CompactionCandidateIntent`** / orphan **`.x1`** / leftover **`.xgc`** whose final cleanup is incomplete (CleanupInProgress / PostSealFinalizing → finish ordered clear + CREATE `.xgc` + GC `.x1` + clear I + unlink `.xgc`; AbandonInProgress / AbandonFinalizing → finish ClrAbandoned + CREATE `.xgc` + GC `.x1` + clear I + unlink `.xgc`; ClrUnauthorized → query-first then proof-valid clear **or** ClrAbandoned on NotFound; Building|Reserved Intent → finish PreSeal-abandon cleanup first; never treat legacy/partial-mig/mid-clear/unauthorized-clr/abandon-in-progress/leftover-Intent/*Finalizing/orphan-`.x1`/leftover-`.xgc` as “no Started”). **Drain complete (round-49/66/67 end state):** continuity OK ∧ (`commit_hw==0` ∨ every seq in `1..commit_hw` Applied) ∧ **no** live `.sj1` ∧ **no** `.jhw` ∧ **no** `.jts` for that candidate ∧ counters consistent ∧ **Intent cleared** ∧ **no orphan `.x1`** ∧ **no leftover `.xgc`** for that build. Mid-clear may still have `.jts` (and `.jhw`) — that is **not** drain-complete. Prevents multi-orphan tip≠baseline false-`Corrupt` and the r48 “need `.jts` after GC deleted them” starvation. Then load `SealIdWatermark`; if `next_candidate_id == UINT64_MAX` or `next_request_id == UINT64_MAX` → fence. Assign `candidate_id = next_candidate_id`, `request_id = next_request_id`; advance `next_* += 1`; **durable-replace + flush watermark** (file + parent). **Also pin** this candidate's **journal baseline bind** = current `CompactionSourceBaseline` `{source_generation=N, baseline_tip_seq, baseline_tip_mac, baseline_key_id}` (round-44 — authenticated; must equal `bridge.prev_*` when Started is later written; must equal Intent baseline). **CREATE_NEW + flush `SealJournalCommitWatermark`** for this `candidate_id` with `highest_committed_journal_seq = 0` **in the same id-reserve step** (round-51 — **not** deferred to first journal Ack; Path B must always have an on-disk seq source). **Then** publish `.x1` seq=1 Building→Reserved (bind ids; no-replace; parent flush) → **REPLACE Intent → Reserved** (recompute MAC; parent flush) — Illegal if Intent missing/not Building/baseline mismatch / `.x1` missing. Path B / journal entries are **illegal** before this durable reservation (entries require `candidate_id` + baseline bind + `.jhw`). Soft RAM / free-running ids are **forbidden**. These ids are consumed for all time even if seal is later abandoned.
      4. **In-flight drain (round-45 B-sticky):** owner runs completion handoff until **zero** outstanding mandatory-capable requests remain and every already-received authoritative response is durable on N (**Path A**) or in the seal journal under the reserved `candidate_id` (**Path B**). RAM-only callback queues are not a drain completion. **B-sticky (P0):** once **any** journal entry for this `candidate_id` is committed, Path A is **illegal** for the rest of this candidate's life until those journals are FIFO-applied (PreSeal-abandon→N or PostSeal→N+1). Path A after Path B inverts observation order on later replay (`wait_generation` / `prior+1` fail-closed or stale evidence). Before any journal exists, Path A remains legal and forces abandon. If tip already ≠ pinned baseline, Path B is also **illegal** (candidate doomed — finish abandon; do not add journals after tip drift).
      5. **Journal reserve / capacity + admit gates (round-38…49):** `kFreezeJournalRoom = 8`; `kPostSealJournalReserve = max(16, kMaxPostSealDeliveryIntents)` (default 16); `kSealJournalMaxEntries = kMaxInFlight + kFreezeJournalRoom + kPostSealJournalReserve`; wire constants from **SealJournalEntryWire** (§10 ABI): **`kSealJournalFixedMetaBytes = 138`**, **`kSealJournalMaxEmbeddedBytes = 4096`**, **`kSealJournalMaxEntryBytes = 4234`**, **`kSealJournalMaxBytes = kSealJournalMaxEntries * kSealJournalMaxEntryBytes`**. Soft PreSeal capacity MUST satisfy **both**: free entry slots (`kSealJournalMaxEntries - journal_entry_count`) ≥ reserve after drain **and** `journal_bytes_used + next_entry_total_bytes ≤ kSealJournalMaxBytes` (must **not** consume PostSeal reserve). **`journal_bytes_used` / `journal_entry_count` (round-47…49):** owner-held counters rebuilt **once** at startup from **live final `.sj1` only** (tombstones / `.jhw` / `.tmp` do **not** count toward soft-cap — separate GC lifecycle below); on Acked publish apply the round-49 `+=` rule (both counters together); on clear `journal_bytes_used -= total_bytes` **and** `journal_entry_count -= 1` only after `.sj1` unlink confirmed — **forbidden** to readdir/sum on every admit; **forbidden** to adjust only one counter. Slot-only soft-cap without byte counter is **illegal**. Live `.jts` per candidate MUST be ≤ `kSealJournalMaxEntries` (else fence — buggy GC / DoS). If drain cannot finish under those bounds → **do not** write `SealExportStarted`; PreSeal-abandon (**FIFO `append_seal_journal_apply` journal→N first**, then reschedule) or fence until space. Under B-sticky, Path A is legal on abandon **only if** no journal entries exist for this `candidate_id`. Capacity counts **all** `candidate_id`s (orphans included). **Journal admission MUST enforce the same gates as `append_seal_journal_apply` before any journal Ack** (round-42…49 — closed loop):
         - encode exactly as **SealJournalEntryWire** (`format_version=1`, `total_bytes=138+payload_len`, packed LE, no padding);
         - `payload_len <= kSealJournalMaxEmbeddedBytes`;
         - `is_seal_journal_embeddable_type(record_type)`;
         - payload schema valid for that type (same pre-Ack checks as the matching live `append_*`);
         - **time provenance (round-43 P0):** capture and persist the observation's original `time_kind` + `recorded_utc_ms` (same rules as L5 §6.1.1 / `FrameTimeKind`: `UnknownBootstrap` ⇒ `recorded_utc_ms==0` and type on Unknown allowlist; `ServerCorrectedUtc` ⇒ non-forged original UTC). **Forbidden** to omit these fields or to fill them from “clock now” at journal Ack / apply / replay.
         - **baseline tip bind (round-44 P1):** every committed entry MUST carry the candidate's pinned `{source_generation, baseline_tip_seq, baseline_tip_mac, baseline_key_id}` and include it in `entry_mac`. Admit MUST refuse if the bind differs from the candidate pin (no mid-candidate baseline drift). A prose-only / MAC-omitted “baseline tip bind” is **illegal**.
         - **`journal_seq` (round-46/47/51):** **sole source** = on-disk `SealJournalCommitWatermark` for this `candidate_id`. Load+MAC-verify `.jhw`; assign `journal_seq = highest_committed_journal_seq + 1` (first Ack ⇒ 1). **Missing / MAC-fail `.jhw` → hard fence** — **forbidden** to invent seq from `max(Applied)`, live `.sj1` readdir, `.jts`, or in-memory counters. Dup or gap vs watermark → refuse Ack / `Corrupt`. If `SealExportStarted` present and `.jhw` absent → fail-closed (same rule; never reconstruct).
         - **`entry_mac` compute (round-46/47 P0):** before publish, MUST set `kek_key_id` to the active journal-signing key, set `store_uuid_*` = CURRENT store uuid, compute `entry_mac = HMAC(KEK[kek_key_id], HY-SEALJRN-v1 domain…)` including `format_version`/`total_bytes`; KEK or store_uuid unavailable → hard fence, retain bytes, **never** Ack.
         - **Publish + commit watermark (round-47…50 P0):** encode into preallocated ≤`kSealJournalMaxEntryBytes` buffer → exclusive-create `*.sj1.tmp` → fsync → **no-replace** publish per §10 ABI (**Windows preferred `CreateHardLinkW` + parent flush + delete tmp**; CREATE_NEW copy only as fallback; **forbidden** replace-`MoveFileExW` / replacing `std::filesystem::rename`). Record `created_final`. If final exists: MAC-verify + **byte-equal** (same fixed buffer) → idempotent (discard tmp, no rewrite); any difference → `Corrupt`, original unchanged. Then parent flush (required even on idempotent path) → load `prev_hw` → durable-replace **`.jhw`** (§10.3 REPLACE allowed **only** for watermark / CURRENT-class files, with **monotonic CAS**; regression → `Corrupt`) → **counter adjust (both or neither):** `+=` both **iff** `(created_final) OR (byte-equal AND prev_hw < journal_seq)`; else skip both → **only then** Ack. Blind “always skip on idempotent” is **illegal**. Acking before final+parent-flush+watermark durable is **illegal**.
         - Failure → **hard fence**, retain transport/handoff buffer ownership, **never** Ack “handled”, **never** commit the journal entry. Do **not** raise the apply cap ad hoc; if larger payloads are ever required, define a segmented reassembly protocol (fixed segment size ≤ 4096, total_len + content hash) in a future revision — not an unbounded raise. Implementers MUST `static_assert` / encode-length-test `kSealJournalFixedMetaBytes == 138` and `kSealJournalTombstoneBytes == 108` — guessing buffer sizes is **illegal**.
      6. **Freeze handoff topology then `SealExportStartedWire` v2 (round-41/42/45/53/54/55/57/64/65):** **Before** writing Started / enabling PostSeal handoff, owner **one-time freezes** `SealJournalIntakeCloseControl` for this `candidate_id`: set `candidate_id`, `registered_producer_mask`, `producer_count`, `ring_id[i]` for each set bit, `topology_frozen=true`. Rules: `producer_count == popcount(mask)`; `1 ≤ producer_count ≤ kMaxSealHandoffProducers`; true-SPSC ⇒ `mask==0b1` and `count==1`; per-producer array ⇒ exactly one bit per live I/O writer ring; every `ring_id[i]` unique among set bits; no bit outside `0..kMax-1`; **unset bits MUST have `ring_id[i]==0`** (fixed-width MAC domain). **Forbidden** after freeze: add/remove bits, change `ring_id`, register unknown slot, duplicate ring — any violation → hard fence. Then, **refuse** if LegacyStarted / `.v2` / `.mig` / `.clr` / `.abd` still present (durable `.abd` ⇒ finish AbandonInProgress through ResumeAuthorized + unlink A first); **require** Intent phase==**Reserved** with matching ids/baseline. Encode packed **`SealExportStartedWire`** (`format_version=2`, `total_bytes=238`, **`kek_key_id`** = active KEK for Started MAC) → `CREATE_NEW` + flush binding the **already-reserved** `candidate_id` / `request_id`, the **frozen topology tuple** (`mask`/`count`/`ring_id[0..kMax)` — MAC-covered under `HY-SEALSTART-v2` with `kek_key_id`), **and** the **exact** journal baseline pin from step 3 (`source_generation` / `baseline_tip_seq` / `baseline_tip_mac` / `baseline_key_id` — MUST already equal `bridge.prev_*` and Intent baseline; **forbidden** to re-sample live CURRENT tip for these fields) plus N+1 seal fields (`new_*`, `content_root`, …). **No second watermark advance.** **Immediately after** durable Started publish: `.x1` seq=2 Reserved→StartedPublished (no-replace; parent flush) → REPLACE Intent → **StartedPublished** (recompute MAC; parent flush). PostSeal begins → call `export_and_wait_ack` **directly** (never via paused tip-export ring). Writing Started before durable watermark reservation **or** before topology freeze **or** without v2 wire/`kek_key_id` **or** without Reserved Intent is **illegal**. Reusing a past id is **illegal**. Started baseline ≠ journal pin / ≠ `bridge.prev_*` / ≠ Intent → refuse write / `Corrupt`.
    - **Breadcrumb / journal flush (round-38…67):** same durability class as §10.3. POSIX: write → `fsync(file)` → `fsync(parent dir)`. Windows NTFS-local: no-replace create / hardlink / CREATE_NEW → `FlushFileBuffers(file)` when content written → **parent dir** `FlushFileBuffers` before treating the name as durable for HW/Ack/counter purposes. Fail-closed on flush failure. **`CompactionCandidateIntent` encode (round-64/65/66/67/71/72):** packed **140B** `HY-COMPINTENT-v1` under `kek_key_id` (wire size unchanged; phase enum adds PostSealFinalizing=3 / AbandonFinalizing=4); CREATE_NEW Building before G (genesis); REPLACE only after durable matching **`CompactionIntentTransitionWire`** `.x1` (176B; `HY-COMPINTENT-X-v1`; no-replace / hardlink class — never REPLACE `.x1`); Intent phase monotonic REPLACE alone does **not** prove no-cross after reboot; terminal clear requires **`CompactionIntentGcAuthorizedWire`** `.xgc` (316B; `HY-COMPINTENT-GC-v2`; no-replace — never REPLACE `.xgc`) before any `.x1` unlink; complete MAC-valid Intent ⇒ durable (Windows parent-flush undecidable). **`SealExportStarted` encode (round-55…65):** packed **`SealExportStartedWire`** — `format_version=2`, `total_bytes=238`, **`kek_key_id`**, MAC under `HY-SEALSTART-v2` with `KEK[kek_key_id]` (never try-all / current-key); greenfield writes `seal-export-started` via CREATE_NEW/no-replace (**refuse** if LegacyStarted / `.v2` / `.mig` / `.clr` / `.abd` present); migration writes **companion** `.v2` + **`.mig` v2** (digests+kek ids; never replace/delete legacy); PostSeal clear via **`.clr` v2** only under **PostSealCommittedProof** after flip+journal drain; authenticated-NotFound Started abandon (with or without C) via **ClrAbandoned** `.abd` A0 first (no-C: present_mask.C=0 + digest_C=0; then GenGone + ResumeAuthorized + unlink A + TipExportProducerResume before any new Started); preallocated fixed buffer; no hot-path heap. **Recovery honesty (round-40/55…58):** a **NativeV2Started** or **MigratedV2Started** (disposition table) **is** PostSeal — recovery **cannot** prove whether the parent-dir flush completed, so it **must not** choose PreSeal-abandon on that basis (may stall under `TransportUnavailable` until query resolves). **`.clr` present:** resume cleanup only. **LegacyStarted** / incomplete mig (V without M): fail-closed (offline companion migrate — §10 ABI); never treat as missing, never invent empty topology, never delete L to “make room.” Only **incomplete/torn length** (or v2 MAC-fail → `Corrupt`) is ignored as non-PostSeal. The old “kill after CREATE_NEW before dir flush → not PostSeal” claim for a complete v2 file is **withdrawn** as undecidable. **Journal finals (round-50):** unlike Started’s undecidable PostSeal choice, journal **must not Ack / must not advance `.jhw` / must not `+=` counters** until the publish recipe’s parent flush returns success in this process — a complete `.sj1` found at recovery without matching HW is handled by the `prev_hw` / continuity rules, not by inventing an Ack.
    - **Path (A) — append-to-N + abandon (PreSeal only; B-sticky):** Legal **only** while (i) no `SealExportStarted` for this candidate **and** (ii) **zero** committed journal entries for this `candidate_id` **and** (iii) Intent phase ∈ {Building, Reserved} (never StartedPublished / PostSealFinalizing / AbandonFinalizing). Immediately `append_*` into live CURRENT (N). Tip advances → baseline mismatch → abandon N+1 (**do not** write `SealExportStarted` / do not export / do not flip; discard/relocate partial `gen-N+1/` under pre-Started GenGone (**if G present**: no-replace rename to T + bind-verify vs Intent baseline; **G absent without T is legal** because Started was never written) → **TipExportProducerResume** if step-1 pause already armed → **CREATE `.xgc` (PreSealAbandonClear) → GC matching `.x1` (if any) → clear Intent → unlink `.xgc` last** → process event → reschedule. Reserved ids stay consumed (watermark already forward when phase was Reserved). Path A after any journal Ack for this candidate is **illegal** → fence/`Corrupt` (do not append; do not Ack “handled” without durable B). Does **not** require `.abd`.
    - **Path (B) — durable seal-side journal (PreSeal or PostSeal):**
      - Location: breadcrumb directory outside store root (same local-FS trust boundary as `OperatorOverrideSidecar`).
      - Capacity: `kSealJournalMaxEntries` / `kSealJournalMaxBytes` / per-entry `kSealJournalMaxEmbeddedBytes` per admit gates above; never drop, **never overwrite** a published final (round-48 no-replace).
      - **Entry metadata / wire (round-37…50):** each **Acked** entry is exactly one published **SealJournalEntryWire** `.sj1` (layout + filename + **no-replace** temp-publish recipe in §10 ABI) **and** a matching advance of **`SealJournalCommitWatermark`**. Fields: `{format_version, total_bytes, store_uuid, kek_key_id, candidate_id, journal_seq, source_generation, baseline_tip_seq, baseline_tip_mac, baseline_key_id, time_kind, recorded_utc_ms, record_type, payload_len, payload_bytes, entry_mac}`. The durable **committed** marker **is** `entry_mac` under `HY-SEALJRN-v1`. `record_type` MUST be embeddable and MUST equal the eventual apply `embedded_type`. `journal_seq` assigned by the **owner actor only** (single-writer): starts at **1**, strict `+1` vs commit watermark. All entries for one `candidate_id` MUST share one identical baseline 4-tuple (else `Corrupt`). Filename `(candidate_id, journal_seq)` MUST equal wire fields (else `Corrupt`). **No commit/Ack without admit gates matching apply.** Local-FS trust boundary (same class as `OperatorOverrideSidecar`): a determined local attacker with FS write is out of scope; accidental damage/loss/overwrite of Acked `.sj1` **must** surface as `Corrupt`, not silent skip.
      - **Journal file disposition (round-47…50):**
        | Condition | Disposition |
        |---|---|
        | `*.sj1.tmp` (never published) | May discard (never Acked) |
        | Final `.sj1` incomplete / short / `total_bytes` mismatch / MAC-fail / filename≠wire / `file_size > total_bytes` **and** `seq ≤ commit_hw` | **`Corrupt`** / hard fence |
        | Final `.sj1` incomplete **and** `seq == commit_hw + 1` **and** no higher evidence | May discard as never-Acked crash-before-publish tail |
        | No-replace publish fails because final exists + byte-equal | Idempotent path (no rewrite); counters per `prev_hw` rule |
        | No-replace would overwrite / final exists + not byte-equal | **`Corrupt`**; original unchanged |
        | Complete length + MAC-ok | Committed evidence; must participate in continuity |
        | Final `.sj1` with `seq ≤ applied_hw` not byte-equal to Applied/`entry_mac` | **`Corrupt`** |
        | `.jts` present **and** `.sj1` present for same seq | Verify `.jts` MAC + `entry_mac` bind to Applied/wire; then unlink `.sj1` (resume clear) — dual presence alone is not `Corrupt` |
        | `.jts` present, no Applied, no matching complete `.sj1` | **`Corrupt`** (orphan clear receipt) |
        | `.jts` incomplete / short / MAC-fail **and** Applied present for that seq | Discard torn `.jts`; recreate from Applied (no-replace) |
        | `.jts` incomplete / MAC-fail **and** no Applied | **`Corrupt`** |
        Store-log “torn last record only” does **not** apply to per-file `.sj1` middles. §10.3 REPLACE recipes MUST NOT be used for `.sj1` / `.jts` / `SealExportStarted` (`.jhw` watermark replace is the explicit exception below).
      - **Continuity / high-water (round-47…50):** per `candidate_id` let
        - `applied_hw` = max `journal_seq` in `SealJournalApplied` index (else 0);
        - `commit_evidence` = complete+MAC-ok `.sj1` ∪ Applied ∪ `SealJournalCommitWatermark` (**tombstones alone do not invent commit_hw** — they are clear receipts bound to Applied/`entry_mac`);
        - `commit_hw` = max seq in `commit_evidence` (watermark alone is sufficient if files were deleted after apply — missing unapplied middles still `Corrupt`).
        Unapplied present set **must** be exactly `{applied_hw+1 … commit_hw}` with contiguous MAC-ok files (or already Applied). Any hole, dup, MAC-fail, replace, or abnormal final under `commit_hw` → `Corrupt`. Must-pass: `Acked seq=1,2 → delete/truncate seq=2 → restart → Corrupt`; `Acked seq=1 → stale tmp → republish same seq different payload → Corrupt, original unchanged`.
      - **Crash-window table (round-50 — normative cuts; each row is a must-pass fault-inject):**
        | Cut after … | On-disk | Recovery / retry |
        |---|---|---|
        | tmp write, before publish | `.sj1.tmp` only | discard tmp; never Acked |
        | hardlink/rename success, before parent flush | final name maybe invisible after power-loss | treat as never Acked; retry publish |
        | parent flush, before `.jhw` advance | complete `.sj1`, `prev_hw < seq` | byte-equal path; advance HW; `+=` both |
        | `.jhw` advanced, before Ack | final + HW | byte-equal; skip `+=`; Ack |
        | Ack, before Applied | committed journal | continuity apply |
        | Applied, before `.jts` | Applied, `.sj1` live | create `.jts` then unlink |
        | `.jts` + parent flush, before unlink | dual `.jts`+`.sj1` | verify; unlink; `-=` after parent flush |
        | unlink + parent flush, before `-=` | `.sj1` gone | `-=` both (or rebuild at restart) |
        | intake-closed SUCCESS | `.jhw` still present; rings empty; Path B prohibited | retain `.jhw` if close unmet/timeout |
        | post-close apply catch-up (round-54) | `applied_hw == commit_hw`; all `.sj1` cleared | GC A illegal while lagging |
        | GC A (`.jhw` gone), before GC B | residual `.jts`; intake closed + catch-up done | resume phase B only; Path B fail-closed |
        | GC B complete | journal drain-complete | authorize `.clr` → `.x1` → Intent PostSealFinalizing → ordered unlink L/V/M → CAPTURE from C → clear C → CREATE `.xgc` (316B evidence) → TipExportProducerResume (idempotent) → GC `.x1` → clear I → unlink `.xgc`; only then new reserve |
        | §10.3 flip → ordered unlink done, before `.xgc` CREATE | I.PostSealFinalizing; Started/C gone; tip@N+1; no Gc | reconstruct/CREATE `.xgc` if absent (gate_absent_at_create if C gone) → TipExportProducerResume (idempotent) → GC `.x1` → clear I → unlink `.xgc` (NOT Corrupt; NOT “ordered unlink → clear Intent”) |
        | §10.3 flip → `.xgc` CREATE durable, before first `.x1` unlink | I + Gc; matching `.x1` still present; L/V/M/C/Started cleared | Mode B (PhysicalCleanupPreconditions hold): GC remaining `.x1` → clear I → unlink `.xgc` last (NOT Corrupt) |
        | §10.3 flip → mid-`.x1` GC under `.xgc` (each seq unlink cut) | I + Gc; partial/gapped `.x1`; gates cleared | Mode B converge (NOT false Corrupt); same cut without `.xgc` → Corrupt |
        | §10.3 flip → `.x1` GC done, crash before Intent unlink | I still PostSealFinalizing; no matching `.x1`; Gc present; gates cleared | Mode B: clear I → unlink `.xgc` last (NOT Corrupt) |
        | §10.3 flip → Intent unlinked, crash before `.xgc` unlink | I gone; Gc present; residual `.x1` optional; gates cleared | Mode B: finish residual `.x1` if any → unlink `.xgc` last (NOT Corrupt) |
        | `.clr` Authorized + proof holds, before unlink | C + L/V/M | CleanupInProgress; resume unlink; never PreSeal/Corrupt |
        | wrongful `.clr` (pre-flip / tip lag) | C + Started; CURRENT still source | ClrUnauthorized; query-first; never unlink |
        | ClrUnauthorized + NotFound + tip==baseline | C + Started; empty journal | ClrAbandoned: A0…GenGone→ResumeAuthorized→`.x1`→Intent AbandonFinalizing→CAPTURE from A→unlink A→CREATE `.xgc` (316B)→TipExportProducerResume (idempotent)→GC `.x1`→clear I→unlink `.xgc` |
        | authenticated NotFound, no C, tip==baseline | Started; empty journal; no C | ClrAbandoned: A0 (mask.C=0,digest_C=0)→A1 no-op→A2…ResumeAuthorized→AbandonFinalizing→CAPTURE from A→unlink A→CREATE `.xgc` (316B)→TipExportProducerResume (idempotent)→GC `.x1`→clear I→unlink `.xgc` |
        | power-cut after A0 (no C), before A2 | A present; Started live; mask.C=0 | AbandonInProgress; A1 no-op; continue A2… |
        | Building Intent durable, before G create | I.phase=Building; no G | PreSeal-abandon: CREATE `.xgc` (zeros terminal mac) → clear I → unlink `.xgc` / retry (not Corrupt) |
        | G fsync, before id-reserve/Started | I.phase=Building; G present; no Started; no A | PreSeal-abandon: clean G + resume if paused + CREATE `.xgc` → clear I → unlink `.xgc`; retry (not Corrupt) |
        | Reserved (ids bound), before Started | I.phase=Reserved; may have journals; no Started | journaled-but-not-Started / PreSeal-abandon; ids not reused; CREATE `.xgc` → GC `.x1` → clear I → unlink `.xgc` |
        | StartedPublished, Started/A abnormally missing | I.phase=StartedPublished; no Started; no A; G and/or T may remain | Corrupt/IoError (not PreSeal) — proves prior Started without terminal Finalizing |
        | Started unlinked on NotFound, no A yet | no Started; no A; I.phase=StartedPublished (or I absent + G\|T) | Corrupt/IoError (not PreSeal) — A0 required first |
        | PostSealFinalizing, Started/C already gone, Intent not cleared | I.phase=PostSealFinalizing; no Started; no A; C may be absent | reconstruct evidence → CREATE `.xgc` if absent → TipExportProducerResume (idempotent) → GC `.x1` → clear I → unlink `.xgc` (NOT Corrupt) |
        | PostSealFinalizing, leftover C/M/V/L, **no** `.xgc` | I.phase=PostSealFinalizing; C and/or Started leftovers; no Gc | resume CleanupInProgress (proof holds); finish unlink; CAPTURE→unlink C→CREATE `.xgc` → TipExportProducerResume (idempotent) → GC `.x1` → clear I → unlink `.xgc` (NOT Corrupt) |
        | PostSealFinalizing + `.xgc` + leftover Started/C (or L/V/M) | I.PostSealFinalizing; Gc present; Started and/or C leftovers | **Corrupt** — early `.xgc` (phase≠cleanup-complete); MUST NOT Mode B GC |
        | AbandonFinalizing + `.xgc` + leftover A (or Started/C) | I.AbandonFinalizing; Gc present; A and/or Started/C leftovers | **Corrupt** — early `.xgc`; MUST NOT Mode B GC |
        | Intent gone + `.xgc` + leftover Started (or C/A) | I gone; Gc present; Started\|C\|A leftover | **Corrupt** — MUST NOT “finish unlink `.xgc` only” |
        | `.clr` Authorized, before Intent PostSealFinalizing | C Authorized; I still StartedPublished; Started live | publish `.x1` seq=3 2→3 → REPLACE Intent→PostSealFinalizing before any Started unlink |
        | `.x1` PostSealFinalizing durable, Intent REPLACE pending | `.x1` to=3; I.phase still StartedPublished | one-step lag: complete Intent REPLACE (NOT Corrupt; NOT accept cross) |
        | illegal PostSeal→Abandon Intent phase / wrong `.x1` edge | I.phase=4 after true PostSeal path / or `.x1` 3→4 | Corrupt (not clear Intent) |
        | AbandonFinalizing, A still present, **no** `.xgc` | I.phase=AbandonFinalizing; A present; `.x1` chain OK; no Gc | CAPTURE from A → unlink A → CREATE `.xgc` → TipExportProducerResume (idempotent) → GC `.x1` → clear I → unlink `.xgc` (NOT Corrupt) |
        | AbandonFinalizing, A already gone, Intent not cleared, **no** `.xgc` | I.phase=AbandonFinalizing; no A; no Started; no Gc | reconstruct evidence → CREATE `.xgc` if absent → TipExportProducerResume (idempotent) → GC `.x1` → clear I → unlink `.xgc` (NOT Corrupt) |
        | ResumeAuthorized, before Intent AbandonFinalizing | A present; A.phase=ResumeAuthorized; I still StartedPublished | `.x1` seq=3 2→4 → REPLACE Intent→AbandonFinalizing while A present; then unlink |
        | `.x1` AbandonFinalizing durable, Intent REPLACE pending | `.x1` to=4; I.phase still StartedPublished | one-step lag: complete Intent REPLACE (NOT Corrupt) |
        | orphan `.x1` without Intent and without `.xgc` | `.x1` present; no I; no Gc | Corrupt |
        | residual `.x1` + Mode-B-eligible `.xgc`, Intent already gone | Gc present; I gone; residual `.x1`; gates cleared | Mode B: finish unlink residual `.x1` + `.xgc` (NOT Corrupt) |
        | Intent clear before `.x1` GC (no `.xgc`) | I gone; residual `.x1`; no Gc | Corrupt |
        | Intent clear before `.x1` GC (with Mode-B-eligible `.xgc`) | I gone mid-order; residual `.x1`; Gc present; gates cleared | non-compliant order if Intent unlinked before all `.x1` gone; recovery Mode B still converges residual |
        | `.xgc` durable, mid-`.x1` GC (e.g. deleted x1[1] from {1,2,3}) | I + Gc; gapped `.x1`; gates cleared | Mode B converge GC (NOT false Corrupt) |
        | mid-`.x1` GC gap, Intent live, **no** `.xgc` | I; gapped `.x1`; no Gc | Corrupt (unauthorized gap) |
        | `.x1` GC done, Intent clear pending, Mode-B-eligible `.xgc` | I still *Finalizing/Reserved; no matching `.x1`; Gc present; gates cleared | Mode B: finish Intent clear then unlink `.xgc` (NOT Corrupt) |
        | `.x1` GC done, Intent clear pending, **no** `.xgc` | I still *Finalizing/Reserved; no matching `.x1`; no Gc | Corrupt (r66 GC-lag without auth **withdrawn**) |
        | `.xgc` present, Intent already gone, no residual `.x1`, gates cleared | Gc only; no Started\|C\|A leftovers | finish unlink `.xgc` (NOT Corrupt) |
        | forged / wrong-bind / dual `.xgc` / dual disposition mismatch | Gc MAC-fail or bind≠I or dual or disposition↔phase illegal | Corrupt |
        | `.xgc` CREATE before terminal phase | Gc present; I.phase=StartedPublished or wrong disposition | Corrupt (illegal early CREATE) |
        | `.xgc` CREATE / present while physical gates still open | Gc present; leftover Started\|C\|A\|L\|V\|M for disposition | Corrupt (early `.xgc`; MUST NOT Mode B) |
        | `.abd` GenGone pending | A + G or T incomplete; I kept until A4 | finish GenGone (no-replace T; G+T missing → Corrupt); then AbandonFinalizing → CAPTURE→unlink A→CREATE `.xgc` → TipExportProducerResume (idempotent) → GC `.x1` → clear I → unlink `.xgc` after A4 |
        | no Intent + (G\|T) present | no I; no Started; no A; G and/or T | Corrupt/IoError — remnant without Intent |
        | Intent phase regression / illegal cross / Mode-A `.x1` chain break | I present; phase decreased or PostSeal↔Abandon or Mode-A seq gap/jump/prev_mac fail (**no** Mode-B-eligible `.xgc`) | Corrupt (Mode B under verified `.xgc` + PhysicalCleanupPreconditions converges authorized gaps — NOT this row) |
        | Reserved/PostSeal/Abandon `.xgc` with `terminal_transition_mac=0` while `.x1` remain | Gc present; zeros mac; matching `.x1` still on disk | Corrupt (zeros legal **only** Building-only PreSealAbandonClear) |
        | Building PreSeal `.xgc` with non-zero `terminal_transition_mac` while no `.x1` | Gc present; non-zero mac; I.Building; no `.x1` | Corrupt (Building-only clear requires zeros) |
        | leftover Intent (incl. *Finalizing) blocks new build | prior I still present | refuse CREATE_NEW Building; finish prior cleanup first (never overwrite) |
        | ResumeAuthorized flushed, before unlink | A present; phase=6; producer paused | re-verify tip==baseline from A; Intent AbandonFinalizing; CAPTURE from A; unlink A; CREATE `.xgc`; TipExportProducerResume (idempotent) → GC `.x1` → clear I → unlink `.xgc` |
        | ResumeAuthorized + tip advanced, A still live | A present; tip＞baseline | non-compliant resume-before-unlink residue; unlink + resume idempotent (equal-or-forward); never tip==baseline self-lock |
        | A unlinked before ResumeAuthorized | no A; producer paused; I not AbandonFinalizing | non-compliant use-after-unlink |
        | rename G→T, before parent flush | T maybe invisible | recovery bind-verifies T or retries; never false GenGone |
        | ClrAbandoned A3b done (A gone), producer still paused | no A; CURRENT at source; I.AbandonFinalizing or cleared | non-compliant if paused; must CREATE `.xgc` (evidence reconstruct if needed) then TipExportProducerResume (A4, idempotent) → GC `.x1` → clear I → unlink `.xgc` |
        | mid-unlink (e.g. M gone), phase lag | C + leftover V/L; I.PostSealFinalizing | raise C phase to match presence; continue order; keep Intent Finalizing |
        | `.clr` ClrPending, C only | C alone; I.PostSealFinalizing | CAPTURE from C → unlink C → parent flush → CREATE `.xgc` → TipExportProducerResume (idempotent) → GC `.x1` → clear I → unlink `.xgc`; then PreSeal may admit |
      - **Tombstone lifecycle (round-48…51):** after each Applied Ack for `seq`:
        1. CREATE_NEW / no-replace write **`SealJournalTombstoneWire`** (`.jts`, same Windows preferred hardlink-from-tmp or CREATE_NEW+flush class as `.sj1`, size 108) with `entry_mac` equal to Applied/`view.entry_mac` → fsync file → **parent fsync**. If `.jts` already exists: MAC-verify + `entry_mac` equal → idempotent (no rewrite); mismatch → `Corrupt`.
        2. Unlink `.sj1` (if still present) → **parent fsync** → **only then** `journal_bytes_used -= total_bytes` **and** `journal_entry_count -= 1` (both; skip both if `.sj1` was already absent and counters already exclude it — never decrement twice for one seq). Decrement **before** parent flush of unlink is **illegal** (NTFS dirent resurrection → soft-cap fail-open).
        3. Crash recovery: if Applied present and `.sj1` already gone but `.jts` missing → may recreate `.jts` from Applied+`entry_mac` (no-replace); never invent `.jts` without Applied; never invent Applied from `.jts` alone.
        4. **Intake-closed barrier (round-51…54 — REQUIRED before GC A):** `.jhw` is the sole Path B seq allocator and **MUST remain** while Path B can still run. “Stop flag + one empty observation” is **illegal**. Waiting on all `kMaxSealHandoffProducers` slots without a frozen mask is **illegal** (true-SPSC hang). Normative close uses **`SealJournalIntakeCloseControl`** (§10 ABI) per `candidate_id`:
           0. **Preconditions:** `topology_frozen==true`; `candidate_id` matches; `popcount(registered_producer_mask)==producer_count≥1`; if `SealExportStarted` present, RAM topology **byte-equal** to Started’s durable topology fields. Else hard fence — do not begin close.
           1. **Arm deadline (round-53 P1):** `close_deadline_steady = steady_now + kSealJournalIntakeCloseDeadlineMs` (steady_clock only). Poll loop capped by deadline **and** `kSealJournalIntakeCloseMaxPollIters` with `_mm_pause()` between spins — **forbidden** unbounded busy-wait. Same-process retry after timeout: re-arm a **fresh** deadline and proceed to step 2 (bump epoch).
           2. **Close epoch:** owner assigns `E = close_epoch.load()+1` (non-zero; wrap of `UINT64_MAX` → hard fence) and `store(E, release)`. Producers `load(acquire)`: if `close_epoch != 0`, **refuse** new `try_push` / new admit; also stop reading new mandatory response bodies (TCP backpressure). Prior `quiesced_ack_epoch == E-1` (or any value `!= E`) is **not** success for this attempt.
           3. **In-flight admit guard:** every producer in the **mask** **before** claiming its bound `ring_id[i]` slot / publishing `tail` does `in_flight_admit_guard.fetch_add(1, acq_rel)`; after publish **or** abandoned push, `fetch_sub(1, acq_rel)`. **Unset-bit** `try_push` / `fetch_add` / publish → **immediate** hard fence (do not wait for deadline). Owner proceeds only when `load(acquire) == 0`.
           4. **Per-producer quiesced ACK (mask-only):** each producer whose bit `i` is set in `registered_producer_mask`, after observing `close_epoch==E` and finishing in-flight admits for `ring_id[i]`, `store(E, release)` into `producers[i].quiesced_ack_epoch`. Owner waits **only** for those bits — **never** for unset slots. ACK from unset bit / wrong `ring_id` / unknown slot → hard fence.
           5. **Double-confirm acquire-drain** on each ring in the frozen set: (a) `tail = load(acquire)`; drain until `head==tail` (popped intent → Path B journal Ack **or** hard fence + retained bytes — **never** drop); (b) `tail2 = load(acquire)`; if `tail2 != tail` or not empty → repeat from (a); (c) re-check `in_flight_admit_guard==0` and every mask bit’s ACK still `==E`.
           6. Zero outstanding mandatory-capable in-flight **network** requests that could still produce Path B outcomes for this candidate.
           7. Owner sets `path_b_prohibited = true` (single-writer). New Path B admits refuse / hard fence until drain-complete.
           **Timeout / cannot complete (round-53/54 P1):** if deadline or max-iters fires before steps 3–6 succeed → **immediately** stop further close polling; **retain `.jhw`**; retain handoff/transport bytes; **hard fence**; **do not** GC A/B; **do not** clear `SealExportStarted`; `path_b_prohibited` stays false (GC illegal) while `close_epoch!=0` still blocks new admits. Retry (same process or recovery) **MUST** bump epoch + re-arm deadline — never treat old ACK as success. Concurrent “last Applied → GC A → handoff” is legal **only** if `.jhw` still exists **or** close **succeeded** under the frozen mask.
        4b. **Post-close apply catch-up (round-54 P0 — REQUIRED before GC A):** intake-close step 5 **may create new** `.sj1` / advance `commit_hw`. After close **SUCCESS**, owner **MUST** run continuity-checked FIFO `append_seal_journal_apply` → tombstone → unlink → counter `-=` until `applied_hw == commit_hw` and no live `.sj1`. **GC A while `applied_hw < commit_hw` is illegal** (orphan journals / silent loss). Pre-close apply passes are allowed for progress but **never** substitute for this catch-up.
        5. **GC phase A (clear watermark):** only when intake-closed (protocol success) ∧ **post-close catch-up done** ∧ `applied_hw == commit_hw` ∧ every seq in `1..commit_hw` has Applied ∧ matching `.jts` ∧ no live `.sj1` → delete `.jhw` → parent fsync. **GC phase B (clear receipts):** after `.jhw` absent ∧ intake-closed, delete all `.jts` for that candidate → parent fsync. **Partial-GC resume (round-49/52):** if Applied∧no `.sj1`∧no `.jhw`∧ residual `.jts` → re-run intake-close protocol (or fail-closed) → **must** resume phase B; do **not** fence solely for “`.jts` after `.jhw` gone.” **Forbidden** while `.jhw` absent: Path B, inventing `journal_seq`, Path A under PostSeal. Only after phase B may Applied frames for that candidate fold under ordinary compaction rules. Wrong order (GC A without intake-closed success **or** without catch-up, drop Applied / GC `.jts` while `.jhw` remains, or clear `.jhw` while `.sj1` remain) → `Corrupt` / hard fence — never silent ignore. Manual cleanup outside this procedure is illegal.
        6. **Drain-complete** (journal side) is no `.sj1` / no `.jhw` / no `.jts` — **not** “still holding matching `.jts` after `.jhw` clear.” **Clear `SealExportStarted` only after PostSeal committed** (CURRENT flipped + `LastRemoteAckedTip` + bridge) **and** journal drain-complete (after GC B) via the **`.clr` v2 protocol** (§10 ABI PostSealCommittedProof) — never after bare journal replay, never pre-flip, never unlink L/V/M without proof-valid `.clr`. Fixed order (round-65/66/67/71): authorize C → **`.x1` seq=3 → Intent PostSealFinalizing** → unlink M (if any) → V (if any) → L → **CAPTURE from C** → unlink C; parent flush after each step; C phase advances monotonic; Intent stays PostSealFinalizing until CREATE `.xgc` (DurableCleanupAuthEvidence) → TipExportProducerResume (idempotent) → GC `.x1` → clear Intent → unlink `.xgc` last. Power-cut mid-clear: recovery resumes CleanupInProgress / PostSealIntentFinalizing only while proof still holds — never PreSeal / never Corrupt solely for leftover names **or** for Started/C already gone under PostSealFinalizing. Wrongful pre-flip `.clr` → ClrUnauthorized → query-first.
      - **Exactly-once apply (round-40…51):** clearing a journal entry is **not** “business append Ack → later applied-marker Ack.” Normative apply on the **target** generation is **one** durable write:
        1. **Classify continuity first** (above). Refuse apply/flip if `Corrupt`.
        2. **Load + MAC-verify** next unapplied `.sj1` (`seq == applied_hw + 1`): require `file_size == total_bytes == 138 + payload_len`; recompute `HY-SEALJRN-v1`; require `journal.store_uuid == CURRENT`; else `Corrupt`.
        3. Build `SealJournalAppliedView{…}` from the verified wire — **copy provenance + baseline + kek_key_id verbatim**; payload span valid until Ack.
        4. **Baseline / ordering verify before apply (round-44/45):**
           - **PostSeal / `SealExportStarted` present:** require entry baseline 4-tuple == `SealExportStarted` / `bridge.prev_*` (full); mismatch → `Corrupt`.
           - **PreSeal-abandon / journaled-but-not-Started:** intra-candidate baseline consistency; tip ≠ pinned baseline with unapplied journals → `Corrupt` (B-then-A). Multi-orphan startup: drain ascending `candidate_id`; tip advance from an **earlier** orphan’s replay is expected and does **not** alone Corrupt a later orphan.
        5. Call **`append_seal_journal_apply(view)`** once. Sink **MUST recompute** `HY-SEALJRN-v1`; index `{candidate_id, journal_seq}` only; idempotent rules unchanged.
        6. **Only after that Ack:** execute tombstone lifecycle steps 1–2 above; when GC is eligible, run intake-closed barrier → **post-close apply catch-up** → GC steps 5–6.
        Forbidden: naked `append_*` as journal apply; Forbidden: post-facto marker; Forbidden: payload-less ABI; Forbidden: silent discard of damaged final `.sj1`; Forbidden: replace-publish of finals; Forbidden: Path A after Path B; Forbidden: opaque-only `entry_mac`; Forbidden: “drain complete requires `.jts` after GC deleted them”; Forbidden: blind skip of both counters on every byte-equal path; Forbidden: `-=` counters before unlink parent flush; Forbidden: hot-path heap for encode/MAC/byte-equal; Forbidden: treating CREATE_NEW copy as preferred Windows path when hardlink works; Forbidden: GC A without intake-closed success; Forbidden: GC A while `applied_hw < commit_hw` / skipping post-close catch-up; Forbidden: inventing `journal_seq` without `.jhw`; Forbidden: Path B when `.jhw` absent; Forbidden: clear `SealExportStarted` before journal drain-complete; Forbidden: unlink L/V/M without proof-valid `.clr`; Forbidden: `.clr` CREATE/CleanupInProgress without PostSealCommittedProof; Forbidden: NotFound abandon leaving unauthorized `.clr` (must ClrAbandoned/`.abd`); Forbidden: authenticated-NotFound Started abandon that clears Started without prior durable A (no-C side-path withdrawn); Forbidden: treat StartedPublished+no-Started+no-A as PreSeal; Forbidden: treat PostSealFinalizing/AbandonFinalizing+no-Started(+no-A) as Corrupt; Forbidden: unlink Started/C before Intent PostSealFinalizing; Forbidden: unlink A before Intent AbandonFinalizing; Forbidden: PostSeal↔Abandon Finalizing cross; Forbidden: Intent REPLACE without prior durable `.x1`; Forbidden: REPLACE live `.x1`; Forbidden: treat Intent phase monotonic REPLACE alone as no-cross proof; Forbidden: unlink `.x1` without durable `.xgc`; Forbidden: CREATE `.xgc` before terminal auth / while physical gates still open / wrong disposition / dual `.xgc` / REPLACE live `.xgc`; Forbidden: Mode B GC when PhysicalCleanupPreconditions fail (early `.xgc`, including Intent-gone + leftover Started|C|A); Forbidden: treat MAC-valid `.xgc` / intent_phase_at_auth alone as Mode B eligibility (r70); Forbidden: clear Intent while matching `.x1` remain; Forbidden: leave orphan `.x1` without Mode-B-eligible `.xgc` converge; Forbidden: leave leftover `.xgc` after Intent gone without finishing unlink (only when PhysicalCleanupPreconditions hold; else Corrupt); Forbidden: treat bare G without Intent as PreSeal; Forbidden: write G before Building Intent; Forbidden: dual/leftover Intent (incl. *Finalizing) without prior cleanup; Forbidden: Intent phase regression / clear Intent before final cleanup; Forbidden: `.abd` on TransportUnavailable/Found/tip≠baseline; Forbidden: greenfield CREATE Started while residual `.abd`/AbandonInProgress; Forbidden: leave tip-export producer paused after ClrAbandoned/NotFound/Path-A abandon; Forbidden: unlink `.abd` while `gen-{new_generation}/` still live; Forbidden: `.clr` against LegacyStarted / kind mismatch; Forbidden: try-all / current-key for Started MAC; Forbidden: Started without `kek_key_id`; Forbidden: single empty-ring observation without close epoch / guard / double-confirm; Forbidden: wait on unset mask bits / all-8 heuristic; Forbidden: PostSeal without topology freeze; Forbidden: invent empty topology from legacy Started; Forbidden: replace/delete legacy Started to install v2; Forbidden: admit PostSeal from `.v2` without complete `.mig`; Forbidden: migrate L↔V without closed equality set; Forbidden: §10.3 abbreviated cleanup as an alternate path; Forbidden: presenting `ordered unlink → clear Intent` or `resume → clear I` / `resume → clear Intent` as a complete terminal PostSeal/Abandon path without CREATE `.xgc` (DurableCleanupAuthEvidence) → TipExportProducerResume (idempotent) → GC `.x1` → clear Intent → unlink `.xgc` last; Forbidden: unbounded close busy-spin; Forbidden: close retry that reuses old epoch ACK.
        Must-pass: prior round-56 cases **plus** every row of the crash-window table; `§10.3 step 4/6 ≡ §10.1/§10.2 full chain`; `close drain → new .sj1 → catch-up apply → only then GC A`; `GC A while applied_hw < commit_hw → illegal`; `Started topology ≠ recovery freeze → fence before demux`; `legacy 192B Started → fence, not empty topology`; `v2 wire 238B + kek_key_id in MAC`; `migrate: closed L↔V equalities + digests in M`; `legacy verify uses single legacy_kek_key_id (no try-all)`; `PostSealCommittedProof → .clr v2 → Intent PostSealFinalizing → ordered unlink → CAPTURE from C → unlink C → CREATE .xgc (316B) → TipExportProducerResume (idempotent) → GC .x1 → clear I → unlink .xgc; power-cut mid-clear / mid-.xgc-GC resumes Mode B when Mode-B PhysicalCleanupPreconditions hold from DurableCleanupAuthEvidence (never false Corrupt/PreSeal; no live C required; early .xgc + leftover Started/C → Corrupt)`; `§10.3 flip → ordered unlink → crash at .xgc CREATE / each .x1 unlink / Intent unlink / .xgc unlink → Mode B converge when .xgc v2 valid AND Mode-B PhysicalCleanupPreconditions hold from DurableCleanupAuthEvidence (not Corrupt; not “ordered unlink → clear Intent”; early .xgc + leftover Started/C → Corrupt)`; `last .clr unlink / last Started gone → crash before Intent clear → I.PostSealFinalizing → restart converges (CAPTURE/reconstruct → CREATE .xgc → TipExportProducerResume (idempotent) → GC .x1 → clear I → unlink .xgc, not fence)`; `Started + zero journal + wrongful .clr + remote Found → query-first (never clear Started)`; `Started → wrongful C → crash → query NotFound → ClrAbandoned (.abd) → new candidate reserve`; `durable .abd + watermark reserved → CREATE Started refused until ResumeAuthorized + unlink A`; `pause → Started → authenticated NotFound → A0 (.abd) → GenGone → ResumeAuthorized → Intent AbandonFinalizing → CAPTURE from A → unlink A → CREATE .xgc (316B) → TipExportProducerResume (idempotent) → GC .x1 → clear I → unlink .xgc → try_push success → remote tip advances` (with-C and no-C; no-C: mask.C=0 + digest_C=0); `.abd unlink → crash before Intent clear → I.AbandonFinalizing → restart converges via .xgc-authorized clear (not fence)`; `Started(no C) → NotFound → clear Started without A → non-compliant`; `G fsync → crash (not yet reserve/Started) → I.Building → CREATE .xgc → clear I → unlink .xgc + retry (not fence)`; `StartedPublished → Started/A missing → Corrupt/IoError`; `PostSealFinalizing → Started/A missing → CREATE .xgc → GC .x1 → clear I → unlink .xgc (not Corrupt)`; `AbandonFinalizing → A missing → resume + CREATE .xgc → GC .x1 → clear I → unlink .xgc (not Corrupt)`; `Reserved PreSeal .xgc with terminal_transition_mac=0 while .x1 remain → Corrupt`; `Building PreSeal .xgc with non-zero terminal_transition_mac while no .x1 → Corrupt`; `illegal PostSealFinalizing→AbandonFinalizing (or wrong jump) + crash → Corrupt (not clear Intent)`; `.x1 durable → crash before Intent REPLACE → recovery completes REPLACE / resumes raise (not false Corrupt / not accept cross)`; `orphan .x1 without Intent and without .xgc → Corrupt`; `PreSeal-abandon / final clear: CREATE .xgc → GC .x1 → Intent → .xgc last`; `mid-GC delete x1[1] from {1,2,3} with I+.xgc + gates cleared → converge (not false Corrupt)`; `same mid-GC gap without .xgc → Corrupt`; `forged/wrong-bind/dual .xgc / dual disposition mismatch → Corrupt`; `Intent gone + Mode-B-eligible .xgc + gates cleared → finish .xgc only (not Corrupt)`; `Intent gone + .xgc + leftover Started|C|A → Corrupt (not finish .xgc only)`; `PostSealFinalizing + early .xgc + leftover Started/C → Corrupt (MUST NOT Mode B)`; `AbandonFinalizing + early .xgc + leftover A → Corrupt`; `phase>Building + no .x1 + no Mode-B-eligible .xgc → Corrupt (GC-lag without auth withdrawn)`; `no Intent + (G|T) → Corrupt`; `A unlinked before ResumeAuthorized → non-compliant use-after-unlink`; `G+T both missing after admitted Started → Corrupt`; `rename→power-cut before parent flush → recovery bind-verifies T`; `T exists wrong bind → Corrupt`; `NotFound abandon leaving unauthorized C → non-compliant self-lock`; `.clr` + LegacyStarted → Corrupt (no unlink)`; `migrate: L kept + V CREATE_NEW + M bind → MigratedV2`; `power-cut at L / V.tmp / V / M.tmp / M → never PreSeal / never new ids`; `V without M → still LegacyStarted fence`; `true-SPSC mask=1 → close does not wait on slots 1..7`; `unset-bit try_push → immediate fence`; `hung producer → deadline → retain .jhw + fence + Started kept + no GC`; `timeout retry bumps epoch`; `Tip@N+1 → full chain → clear Started only via .clr after journal drain-complete`; `Started + missing .jhw → Path B fail-closed`.
      - **PreSeal full (soft region):** `Failed` + fence seal until drained **or** abandon candidate.
      - **PostSeal full (including reserve exhaustion):** hard **process fence** — retain observation bytes in the handoff slot / transport buffer; **never** path A; **never** Ack “handled”; **never** early-flip solely to free journal space. Flip+drain remains the only legal clearer, and only after seal tip rules succeed.
      - Caller observes `Acked` only after journal fsync of a committed entry that passed admit gates.
      - **PostSeal network→owner handoff (round-37/39 P0):** delivery-intent ring(s) hold **unauthenticated** intents only (raw response identity + span into a preallocated buffer pool). Intent presence is **not** durability. The owner (or owner-driven poll inside `export_and_wait_ack`) must journal (B) before any upper-layer “handled”.
        - **SPSC discipline (round-39/52):** “I/O threads may push” into **one** SPSC is **forbidden** (multi-writer races on `tail` → lost 429/fill). Legal shapes only:
          1. **True SPSC:** exactly one named producer — either the owner (polls sockets itself during PostSeal) **or** a single `SealCompletionDemux` thread that is the sole writer; other I/O threads may only hand buffers to that demux via private ownership transfer, never concurrent `try_push` on the same ring; **or**
          2. **Per-producer SPSC array:** one fixed-capacity SPSC per I/O thread; owner round-robins consume. Still one writer per ring.
        Capacity of the consumed set ≤ `kPostSealJournalReserve`. When any ring is full: that producer **stops reading** new mandatory-capable response bodies (TCP backpressure) and **retains buffer ownership** — never overwrite, never drop, never steal another producer’s slot. **Intake-close (round-52…55):** every legal producer shape MUST be named in the **frozen** `registered_producer_mask` and participate in `SealJournalIntakeCloseControl` (mask-only quiesced ACK + admit guard + double-confirm drain + steady deadline) → **post-close catch-up** before GC A — bare “paused + size()==0” or “wait all 8 slots” or “close → GC without catch-up” or “§10.3 shorthand cleanup” is **non-compliant**.
      - **Recovery (normative — round-36…70):**
        0. Rebuild `journal_bytes_used` / `journal_entry_count` once from live `.sj1` only; load all `.jhw` (monotonic check) and `.jts` (MAC + `entry_mac` bind; torn `.jts` per disposition table); discard only `*.sj1.tmp` / `*.v2.tmp` / `*.mig.tmp` / `*.clr.tmp` / `*.abd.tmp` / `*.intent.tmp` / `*.x1.tmp` / `*.xgc.tmp` (and short never-Acked fallback finals); run **continuity + no-replace disposition** for every candidate. Re-init `SealJournalIntakeCloseControl` (`close_epoch=0`, `path_b_prohibited=false`) — prior-process quiesce is void. Classify Started/Intent per §10 ABI disposition (**AbandonInProgress** / **CleanupInProgress** / **ClrUnauthorized** / **NativeV2Started** / **MigratedV2Started** / **LegacyStarted** / incomplete-mig / torn / **PreSealIntentAbandoned** / **PostSealIntentFinalizing** / **AbandonIntentFinalizing**). Discard `*.abd.tmp` / torn Intent. **Before trusting any Intent.phase path:** verify optional `.xgc` + `.x1` chain (Mode B only when MAC-valid `.xgc` v2 **AND Mode-B PhysicalCleanupPreconditions from DurableCleanupAuthEvidence + live gate absence; never re-read deleted `.clr`/`.abd`; AND PhysicalCleanupPreconditions(`terminal_disposition`) hold** — converges authorized gaps; early `.xgc` + leftover Started|C|A|L|V|M → Corrupt, MUST NOT Mode B; Mode A gap/jump/cross/prev_mac break / orphan `.x1` without Mode-B-eligible `.xgc` → Corrupt; Intent gone + Mode-B-eligible `.xgc` + gates cleared → finish residual `.x1` + unlink `.xgc`; Intent gone + `.xgc` + leftover Started|C|A → Corrupt, not “finish `.xgc` only”). Evaluate `.abd` first (MAC under wire `kek_key_id`): present → **AbandonInProgress** resume ClrAbandoned only (ensure Intent→AbandonFinalizing while A present; keep Intent until A4 then **CAPTURE from A → unlink A → CREATE `.xgc` (316B) → TipExportProducerResume (idempotent) → GC `.x1` → clear I → unlink `.xgc`**). Else load Intent (MAC under wire `kek_key_id`): **Building|Reserved + no Started + no A** → **PreSeal-abandon** (NOT Corrupt) — verify bridge/baseline vs I, clean G under pre-Started GenGone, TipExportProducerResume if pause armed, **CREATE `.xgc` → GC `.x1` (if any) → clear I → unlink `.xgc`**. **PostSealFinalizing + no A + no early .xgc** → finish cleanup (verify CURRENT/tip/bridge/journal; resume .clr if present) → **CAPTURE/reconstruct → CREATE .xgc (316B DurableCleanupAuthEvidence) → TipExportProducerResume (idempotent) → GC .x1 → clear I → unlink .xgc** (**NOT Corrupt** even if Started/C already gone). **PostSealFinalizing + .xgc + leftover Started|C|L|V|M** → **Corrupt** (early .xgc; MUST NOT Mode B). **AbandonFinalizing + no early .xgc** → CAPTURE/finish unlink A if present → **CREATE .xgc (316B) → TipExportProducerResume (idempotent) → GC .x1 → clear I → unlink .xgc** (**NOT Corrupt** even if A already gone). **AbandonFinalizing + .xgc + leftover A|Started|C** → **Corrupt**. **StartedPublished + no Started + no A** → **Corrupt/IoError** (non-terminal only). **No Intent + no Started + no A + (G|T)** → **Corrupt/IoError**. Else evaluate `.clr` under **wire `kek_key_id`** + **PostSealCommittedProof** (CURRENT / `LastRemoteAckedTip` / bridge / journal). **CleanupInProgress** only when proof holds — then ensure Intent PostSealFinalizing before further Started unlinks; resume ordered unlink (**presence≫stale phase**); after `.clr` clear completes → **CAPTURE/reconstruct → CREATE `.xgc` (316B) → TipExportProducerResume (idempotent) → GC `.x1` → clear Intent → unlink `.xgc`**; **FORBIDDEN** PreSeal / new id reserve / Corrupt solely for leftovers. **ClrUnauthorized** / draft 176B: **do not unlink** Started; keep L/V/M; force query-first PostSeal (NotFound + tip==baseline + empty journal → **ClrAbandoned** via `.abd`, never leave C). Early `.clr` with live journal → Corrupt. **Before** starting transport/demux for an incomplete PostSeal candidate: verify Started MAC under **wire `kek_key_id`** (never try-all / current-key); **rebuild and freeze** topology from the **admitted v2 wire** (L for NativeV2, V for MigratedV2) — **not** from live config alone; config may only supply the freeze when Started is absent (PreSeal). Freeze ≠ Started topology → hard fence (do not start demux). **LegacyStarted** / V-without-M → hard fence (offline companion migrate first; single `legacy_kek_key_id`). If admitted v2 Started exists **and** `.jhw` is missing: **Path B fail-closed**; may only **re-run intake-close** (with deadline) + **catch-up apply if any `.sj1` remain** and resume GC B when Applied∧no `.sj1`∧ residual `.jts`. If Started exists, `.jhw` missing, and unapplied work would require Path B → hard fence/`Corrupt`. **Resume partial GC** only after successful close + catch-up; keep all Started names until **journal** drain-complete **and** `.clr` protocol finishes (Intent PostSealFinalizing raised before last Started unlink).
        1. If **NativeV2Started** or **MigratedV2Started** → **force PostSeal** for that `candidate_id` / `request_id` (use L or V wire respectively). **Watermark invariant (round-41):** load `SealIdWatermark`; require `next_candidate_id > started.candidate_id` **and** `next_request_id > started.request_id`. If watermark missing/lagging → fence/`Corrupt` or **forward-only** repair that advances watermark past those ids **without reallocating them** — **never** assign the same ids to a new attempt. Then call `query_seal_by_request_id` → **`SealQueryStatus`**. **Do not** mint a new `request_id`/`candidate_id` or send a second seal for the same breadcrumb — recovery is query-first. Idempotent re-export of the **same** `request_id` is allowed only under `TransportUnavailable` / unknown remote state (then query again). On **`Found`**: verify returned `GenerationSeal` field-by-field binds to the breadcrumb **and** local gen-N+1: `store_uuid`, `request_id`, `content_root`, `final_seq`/`final_tip_mac`/`new_key_id`, **Started baseline 4-tuple == `bridge.prev_*` (full)**, N+1 tip matches seal; MAC verifies → tip persist → flip → **full cleanup chain** (apply → intake-close → **post-close catch-up** → GC A/B → journal drain-complete → **`.clr` authorize → `.x1` → Intent PostSealFinalizing → ordered unlink M→V→L → CAPTURE from C → unlink C → CREATE `.xgc` (316B; `HY-COMPINTENT-GC-v2`) → TipExportProducerResume (idempotent) → GC `.x1` → clear Intent → unlink `.xgc`**) → **only then** Started names **and** Intent/`.x1`/`.xgc` are gone (Path B during this chain requires `.jhw` until intake-close success + GC A). On **`TransportUnavailable`**: **Keep** Started (incl. L+V+M); fence; re-query. On **`NotFound`**: may abandon **iff** N tip still equals Started/bridge baseline 4-tuple **and** journal empty for that candidate (else `Corrupt`). Whether C present or absent → **must** run **ClrAbandoned** (CREATE `.abd` A0 **before** any unlink of C/Started/G; no-C ⇒ `present_mask.C=0` + `digest_C=0`; then A1…A4: unlink C if any → unlink Started M→V→L → GenGone → ResumeAuthorized → `.x1` → Intent AbandonFinalizing → CAPTURE from A → unlink A → CREATE `.xgc` (316B) → TipExportProducerResume (idempotent) → **GC `.x1` → clear Intent → unlink `.xgc`**). Naked NotFound clear that leaves C **or** clears Started without A is **non-compliant** (self-lock / lost bind / false PreSeal). Ids never reused. On **`Corrupt`** → `Corrupt`.
        2. **Journaled-but-not-Started / PreSeal abandon (round-42…52/64):** no admitted v2 Started, Intent phase ∈ {Building, Reserved} (or Reserved-equivalent journal evidence with Intent), and/or journal evidence exists → drain **ALL** candidates ascending by `candidate_id` through full tombstone + intake-close protocol + **catch-up** + GC chain (partial-GC resume) + **CREATE `.xgc` → GC `.x1` → clear Intent → unlink `.xgc` last** after each candidate’s pre-Started cleanup. Per candidate: continuity `1..commit_hw` + MAC-verify; intra-candidate baseline vs Intent; tip≠baseline with unapplied → `Corrupt` (B-then-A); else FIFO-replay onto N; clean G under pre-Started GenGone; **never** invent Started; **never** reclaim ids; **never** skip holes; **never** replace-publish; **never** Path B without `.jhw`; **never** treat StartedPublished as this case. (**LegacyStarted** / incomplete mig is not this case — fail-closed, not PreSeal.)
        3. **PostSeal** (`Found` / tip@N+1): require N tip == `bridge.prev_tip_*`; tip persist; flip; **then** **full cleanup chain** onto N+1; clear Started **only** via **proof-valid `.clr` v2** after journal drain-complete (no `.sj1` / no `.jhw` / no `.jts`); **ClrUnauthorized** `.clr` must not short-circuit this path; discard residual gen-N outbox without send.
        4. **Never** replay journal onto N while an admitted v2 Started exists for that candidate (except tip-guarded `NotFound` abandon).
        5. Torn Started lengths may be ignored as non-PostSeal; v2 complete-but-MAC-fail → `Corrupt`; LegacyStarted / V-without-M → fence (companion migrate). Final `.sj1` disposition is the round-47…49 table — **not** “discard like torn log,” **not** replace-capable.
      - Power-loss after journal Ack + before store append: recovery replays (continuity + MAC recompute + byte-equal republish). Power-loss with only volatile memory: must not occur under a compliant impl.
    - Non-mandatory work during seal (new submit not yet on the wire) may be refused with retryable busy without journaling.
  - Deep traps closed: tip@N+1 with `CURRENT=N` does not make a volatile buffer safe; PostSeal path A is unsafe; **missing `SealExportStarted`** made remote-persisted/local-unacked look PreSeal; **journal-full + flip-only clear** without pre-quiesce/reserve deadlocked mandatory completions; **Unavailable≠absent** (round-38) refined to **`SealQueryStatus`** (round-39); **request_id reuse** / **multi-writer SPSC** (round-39); **two-phase journal marker** / **stale outbox after seal** / **undecidable Windows dir-flush PreSeal** (round-40); **de-dup key included entry_mac** / **payload-less apply ABI** / **cross-file “same barrier” watermark+Started** (round-41); **Path B before durable candidate_id** / **journal Ack > apply size cap** / **open embedded_type** (round-42); **journal omitted time_kind/recorded_utc_ms → replay re-stamp** (round-43); **MAC-omitted baseline tip bind → false security** (round-44); **Path B then Path A order inversion** / **Started baseline re-sample** / **dual committed-marker MAC** (round-45); **opaque entry_mac without recompute** / **guessed FixedMetaBytes=128** / **admit without MAC compute** (round-46); **Acked `.sj1` middle silently discarded / no commit_hw continuity** (round-47); **replace-capable `.sj1` publish / undefined tombstone GC** (round-48); **drain-complete required live `.jts` after GC deleted them** / **blind skip `+=` on every byte-equal** / **entry_count never maintained** / **no partial-GC resume** (round-49); **Windows dual publish OR without preferred hardlink** / **`-=` before unlink parent flush** / **no crash-window table** / **hot-path heap for byte-equal** (round-50); **GC A deletes `.jhw` while PostSeal intake still open / Started still present** (round-51); **crash-window cleared Started after bare journal→N+1** / **intake-close without linearizable producer quiesce** (round-52); **close wait-set = all 8 slots without registered_mask** / **close without steady deadline** (round-53); **apply→close→GC without post-close catch-up** / **RAM-only topology vs Started after config drift** / **close retry reusing old epoch ACK** (round-54); **§10.3 abbreviated PreSeal/cleanup bypassing §10.1 chain** / **Started without format_version/total_bytes → legacy/truncated ambiguity** (round-55); **v1→v2 migrate via replace/delete-legacy deadlock or PreSeal window** (round-56); **Started/mig without kek_key_id → try-all after rotation** / **non-atomic L/V/M clear → false Corrupt** / **ellipsis L↔V bind** (round-57); **`.clr` without PostSealCommittedProof → pre-flip Started erase / generation fork** (round-58); **ClrUnauthorized + NotFound without ClrAbandoned → permanent self-lock** (round-59). **ClrAbandoned without TipExportProducerResume → tip hard-lag** / **ClrAbandoned without GenGone → residual gen-N+1/** (round-61). **Resume after unlink A → unconstructible recovery** / **GenGone false idempotency G+T missing** (round-62). **NotFound-without-C clear Started before A → lost bind / false PreSeal / late Found undetectable** (round-63). **r63 bare-G Corrupt false-positive on legal PreSeal-build crash** / **Intent vs tip drift during Building** / **dual leftover Intent blocking new build** / **Intent phase regression** / **Intent clear too early vs forever-blocking** / **Reserved crash before Started (ids not reused)** / **Intent clear timing vs `.abd` A clear** (round-64). **r64 StartedPublished+no-Started+no-A false-Corrupt on nearly-finished PostSeal/abandon** / **phase monotonicity PostSeal↔Abandon cross** / **completion_kind vs phase duplication (phase alone)** / **leftover Finalizing blocking new build** / **CleanupInProgress `.clr` presence≫phase vs Intent PostSealFinalizing** / **AbandonFinalizing without A** / **PostSealFinalizing with leftover C/M/V/L** (round-65). **Intent phase-cross not crash-verifiable from REPLACE-only `phase`** / **`.x1` before Intent REPLACE** (round-66). **multi-file `.x1` GC gap false-Corrupt without `.xgc`** / **r66 GC-lag without auth** (round-67). **Recovery/crash-window abbreviations reintroducing clear-I without `.xgc`** (round-68). **§10.3 abbreviated `ordered unlink → clear Intent` omitting `.xgc` tail** (round-69). **early MAC-legal `.xgc` Mode B skipping PhysicalCleanupPreconditions** / **`intent_phase_at_auth=PostSealFinalizing` ≠ cleanup-complete** / **Intent-gone+`.xgc`+leftover Started finish-`.xgc`-only false converge** (round-70).
- **Checkpoint fold bound (round-12 P1)**: for a single long-lived non-exchange-final COID (`!is_exchange_final(state)` — see §6.1.3's fix above; includes an escalated order awaiting operator resolution, not just a resting live one), poll/fill/reconcile events are folded at compaction into at most `kMaxCheckpointFramesPerOrder = 8` retained frames (latest state + latest fill progress + latest attempt/poll counters + pin refs). Without this, a multi-day resting order's per-second poll trail can alone hit the hard cap and fence the whole system.
- **Hard cap reached**: `append_*` → `Failed` → fence. No silent drop-oldest.
- **Generation ID exhaustion (round-34 P1):** if `CURRENT` generation is `UINT32_MAX`, compaction that would need `target = source + 1` is **refused**; permanent fence + offline store migration (new store uuid / operator procedure). Never wrap generation to 0.

### 10.2 Rollback threat model + external anchor — fixes round-10/11/12 P0

What local HMAC + tip anchor detect vs. what they cannot (whole-store rollback) is unchanged from revision 10. Mechanisms:

1. **Store-identity breadcrumb** (unchanged): outside store root; delete-everything with breadcrumb present → `IoError`.
2. **External tip anchor + backpressure:** every locally `Acked` append enqueues exactly `{store_uuid, generation, sequence_number, mac_tip, key_id}` to `ExternalAnchorClient::export_tip_and_wait_bounded`. The fixed preallocated FIFO is bounded by `kExternalAnchorSoftLagFrames=64` / `kExternalAnchorSoftLagMs=5000` (alarm) and `kExternalAnchorHardLagFrames=256` / `kExternalAnchorHardLagMs=30000` (fence before admitting another append).

   **Sequence comparison is lexicographic on `(generation, sequence_number)`, never bare `sequence_number` — fixes this round's P0.** An earlier revision's "lower sequence... is `Corrupt`" rule, read literally, contradicts compaction by construction: `GenerationBridgePayload::new_genesis_seq` is documented as "usually 0" (§10.2 point 4 / L5 §6.1.3), so the very first export in a freshly-compacted generation `N+1` (sequence 0) would look like a rollback relative to generation `N`'s last-exported sequence (routinely in the tens of thousands) under a bare numeric comparison — flagging every legitimate compaction as corruption. Fixed: all "must not go backward" comparisons in this section — the FIFO's own ACK-ordering check, and recovery's "local tip vs. external tip" comparison — compare `(generation, sequence_number)` as a tuple, ordered first by `generation` then by `sequence_number`: `(N+1, 0) > (N, 50000)`. Within a single generation (`generation` unchanged), the existing rule stands unmodified: a lower `sequence_number`, a changed tip for an existing sequence, a wrong UUID/key, or equivocation is `Corrupt`. **A generation increase is never itself evidence of rollback** — it is only accepted when backed by a verified `GenerationBridge` chain (point 4 below); an unexplained generation jump with no corresponding bridge is `Corrupt` exactly as a same-generation sequence regression is.
   - ACK is idempotent and must echo the exact `(store_uuid, generation, sequence_number, mac_tip, key_id)` tuple. Recovery reads the latest external tip and requires a local chain that is equal-or-forward under the tuple ordering above; a local tip that is `(generation, sequence_number)`-older than the external tip, without a verified forward bridge chain covering the gap, is rollback/`Corrupt`. The actor may batch network exports, but may not allocate, drop, reorder, or unboundedly queue them.
3. **OperatorOverride sidecar — fixes round-12 P0 (self-lock); degraded-baseline binding fixed round-19 P0**:
   - Admission evidence is written via `OperatorOverrideSidecar` to the **breadcrumb directory** (outside the store), by an offline operator tool, with HMAC under KEK. Recovery reads the sidecar **without opening the store for append**.
   - Admission algorithm: `recover_control_plane` / `recovery_scan` return `ExternalAnchorUnavailable` → startup checks sidecar → verifies canonical MAC under the recorded `kek_key_id`, a **fresh `/time` probe** against `expires_utc_ms`, unused nonce/tombstone absence, `store_uuid`, `local_tip_seq`/`local_tip_mac`/`local_generation`, **and** `last_remote_acked_*` matching on-disk `LastRemoteAckedTip` → if valid, mirrors via `append_operator_override`; only an ACKed mirror may create `consume_after_successful_admission(nonce)`. Then §10.2.1 degraded path runs with that bound baseline (not an empty ring).
   - If computing inherited unanchored backlog at issue time already exceeds hard-lag, the operator tool **must** set `admit_mode = ReadOnlyDrain`; an `AppendAllowedIfUnderHardLag` override presented against an over-limit backlog is refused at admission.
   - `DurableRecordType::OperatorOverride = 6` + `append_operator_override` exist **only** for that post-admission mirror — never as the admission gate.
   - **Trust-boundary limitation — fixes this round's P0 (an explicit acknowledgment, not a stronger guarantee this design cannot honestly provide)**: the override evidence and its consumption tombstone are both persisted **locally, on the same host, under the same breadcrumb directory**, protected only by an HMAC under a locally-held KEK. This gives replay protection against *this process* re-consuming the same override, and against *accidental* local corruption or duplication — it does **not** give protection against an attacker (or an operator error) with local filesystem write access who deletes both the override evidence file and its tombstone: nothing outside this host attests that a given override nonce was ever consumed, so a deleted-and-recreated override could be "replayed." Building a genuine defense against that (an external monotonic counter service, or WORM storage with a retention lock this host cannot itself clear) is real infrastructure this spec does not invent — doing so casually, without operating experience with the chosen service, would trade one set of untested assumptions for another. This is therefore an **explicitly accepted, documented trust boundary**: `OperatorOverrideSidecar` protects against accidental replay and single-process misuse, not against a determined local attacker with filesystem access to the host running L5. The one mitigation this design does provide within that scope: `local_tip_seq`/`local_tip_mac`/`local_generation` bind an override to the exact store state it was issued against (already part of `OperatorOverridePayload`), so replaying an old override after the store has legitimately advanced (a new compaction, a new generation) is at least detectable — the bound tip would mismatch the store's actual current tip — even though the sidecar mechanism itself cannot prevent the replay attempt from being tried. Any deployment whose threat model includes a determined local attacker must treat `OperatorOverrideSidecar` admission as out of scope for that threat and provide its own external attestation layer — this spec's honest position is that it does not solve that problem, not that it silently does.
4. **GenerationBridge + external seal-before-CURRENT — … (round-40 P0); journal de-dup key / View ABI / watermark-before-Started (round-41 P0); id-before-Path-B / journal↔apply closed loop / embeddable allowlist (round-42 P0/P1); seal-journal time provenance (round-43 P0); authenticated baseline bind (round-44 P1); B-sticky + Started/Found baseline copy (round-45 P0/P1):**
   - Compaction writes generation `N+1` including a leading `GenerationBridge` frame whose payload binds `{prev_generation, prev_tip_seq, prev_tip_mac, prev_key_id}` to `{new_generation, new_genesis_seq}`, keyed by `new_key_id`. **`prev_tip_*` + `prev_key_id` MUST equal `CompactionSourceBaseline`** pinned at retain-scan start (§10.1). If `N == UINT32_MAX`, refuse compaction (generation exhaustion). **Before any `gen-N+1/` byte:** durable **`CompactionCandidateIntent` Building** (§10.1 / §10 ABI).
   - After every retained frame, final anchor and content root of `N+1` are fsynced, **re-verify CURRENT tip == baseline** (and == `bridge.prev_tip_seq/mac/key_id` and == Intent baseline); on mismatch **PreSeal-abandon** (§10.1 — Intent still Building|Reserved; no `SealExportStarted` yet; clear Intent after G cleanup). Explicit equalities: `baseline == {tip_seq, tip_mac, tip.key_id}`; `bridge.prev_* == baseline`; `Intent.baseline_* == baseline`; `GenerationSeal.key_id == bridge.new_key_id ==` N+1 final tip `key_id` (do **not** compare seal `key_id` to `prev_key_id`).
   - **Content-root (round-19/34/38):** `content_root = SHA-256` over the concatenation, in ascending `sequence_number` order, of every retained frame's **full on-disk framed bytes** in gen-N+1 (format_version || record_type || sequence || time_kind || recorded_utc_ms || length || payload_bytes), including `CompactedFreezeWaitEvidencePayload` with `source_baseline_key_id`. No dual-frame alternate. `SealExportStarted.content_root` MUST equal `GenerationSeal.content_root`. Post-flip journal applies via `append_seal_journal_apply` append **after** seal content-root is fixed — they advance N+1 tip under ordinary append rules and are **not** retroactively part of the sealed content-root.
   - **Enter PostSeal (round-37…65):** perform §10.1 PreSeal admission (**pause tip producer + empty `ExportOutboxRing` + finish tip RPCs**, then quiesce, then **durable watermark reserve of candidate_id+request_id + pin journal baseline = CompactionSourceBaseline + REPLACE Intent→Reserved**, then in-flight drain under **B-sticky**, then journal soft-cap check, then **freeze `SealJournalIntakeCloseControl` topology**). Then `CREATE_NEW`+fsync packed **`SealExportStartedWire` v2** (238B, **`kek_key_id`**) onto `seal-export-started` (greenfield; refuse if LegacyStarted / `.v2` / `.mig` / `.clr` / `.abd` still present; require Intent Reserved) reusing those reserved ids, the **exact baseline pin**, **and the frozen topology tuple** (**no** second watermark advance; **no** live-tip re-sample for baseline fields). **Immediately REPLACE Intent→StartedPublished.** **Only then** call `ExternalAnchorClient::export_and_wait_ack(seal)` **directly**. `export_and_wait_ack` MUST refuse to send if breadcrumb missing/mismatched. Path B / journal before durable candidate reservation is illegal. PostSeal without topology freeze **or** Started without v2/`kek_key_id`/topology MAC bind is illegal. Legacy upgrade uses companion+mig only (§10 ABI) — never replace L; never try-all KEK. §10.3 must not redefine a shorter PreSeal.
   - **After seal Ack, BEFORE `CURRENT` flip (round-20/21/36):** again verify tip == baseline; durably persist `LastRemoteAckedTip = {store_uuid, new_generation, final_seq, final_tip_mac, key_id}` under §10.2.1 monotonic CAS / single-writer. Mandatory inputs: path **B only** into reserved journal / owner handoff (true SPSC / per-producer SPSC). Discard residual gen-N outbox tuples without send.
   - Tip-persist failure: **do not** flip; fence; retry only if N tip == `bridge.prev_tip_seq/mac/key_id`. Tip drift after `SealExportStarted` → `Corrupt` (not keep-N abandon).
   - After tip persist: flip `CURRENT` → N+1; fsync parent; run **full cleanup chain** for `candidate_id` onto N+1; clear `SealExportStarted` only after PostSealCommittedProof + journal drain via **`.clr` v2 protocol** (Intent **PostSealFinalizing** before Started unlink); then freeze projection / live appends.
   - **Recovery rule (round-34…68):** flip to N+1 requires archived N final tip == `bridge.prev_tip_seq/mac/key_id`. Verify `.xgc`/`.x1` (Mode A/B) before trusting Intent.phase. If `.abd` present → **AbandonInProgress** resume ClrAbandoned (Intent AbandonFinalizing while A present; CAPTURE from A → unlink A → **CREATE `.xgc` → TipExportProducerResume (idempotent) → GC `.x1` → clear Intent → unlink `.xgc`**). Else if Intent **PostSealFinalizing** + no A → finish clear + CAPTURE/reconstruct → **CREATE `.xgc` → TipExportProducerResume (idempotent) → GC `.x1` → clear I → unlink `.xgc`** (**NOT Corrupt**). Else if Intent **AbandonFinalizing** → CAPTURE/finish unlink A if needed → **CREATE `.xgc` → TipExportProducerResume (idempotent) → GC `.x1` → clear I → unlink `.xgc`** (**NOT Corrupt**). Else if Intent **StartedPublished** + no Started + no A → **Corrupt/IoError**. Else if Intent **Building|Reserved** + no Started + no A → **PreSeal-abandon** (NOT Corrupt; CREATE `.xgc` → GC `.x1` if any → clear I → unlink `.xgc`). Else if no Intent + (G|T) + no Started + no A → **Corrupt/IoError**. Else if `.clr` present → evaluate **PostSealCommittedProof**: hold → CleanupInProgress resume (ensure Intent PostSealFinalizing; after `.clr` done → CAPTURE→unlink C→CREATE `.xgc` → TipExportProducerResume (idempotent) → GC `.x1` → clear Intent → unlink `.xgc`); fail → **ClrUnauthorized** + query-first (never unlink; NotFound → ClrAbandoned). If no **NativeV2Started** / **MigratedV2Started** → PreSeal rules (+ Intent + continuity/`SealJournalCommitWatermark` + B-sticky) **unless** `LegacyStarted` / incomplete mig (V without M) present (fail-closed companion migrate). If admitted v2 present → §10.1 recovery (`SealQueryStatus` + watermark past Started ids + full baseline 4-tuple Found bind + **full cleanup chain**); verify Started under wire `kek_key_id`. Never journal→N while admitted v2 Started exists (except authenticated-`NotFound` abandon **with tip==baseline**). Never flip/apply across a journal hole under `commit_hw`.
   - **Normative PostSeal cleanup chain (round-52…69 — every crash/recovery cut):** `tip persist` → `flip CURRENT` → continuity-checked `append_seal_journal_apply` → tombstone → unlink `.sj1` → **intake-close protocol** → **post-close apply catch-up** → GC A → GC B → **journal drain-complete** → **PostSealCommittedProof** → **`.clr` v2 authorize** → **`.x1` → Intent→PostSealFinalizing** → ordered unlink M→V→L → CAPTURE from C → unlink C → **CREATE `.xgc` → TipExportProducerResume (idempotent) → GC `.x1` → clear Intent → unlink `.xgc` last**. Abbreviated “journal→N+1; clear Started” / “close → GC without catch-up” / “unlink L/V/M without proof-valid `.clr`” / “`.clr` before flip” / “clear Intent while still StartedPublished after Started/C gone” / “GC `.x1` without prior `.xgc`” / “TipExportProducerResume → clear I without `.xgc`” / “ordered unlink → clear Intent” / “resume → clear Intent” without `.xgc` / §10.3 Cross-consistency shorthand omitting `.xgc` is **withdrawn**.
   - **Crash windows:**
     - Intent Building, G fsynced, no reserve/Started: **PreSeal-abandon CREATE `.xgc` → clear I → unlink `.xgc` + retry** (not Corrupt).
     - Watermark reserved (Intent Reserved), no Started yet (may have journal entries): ids consumed; **journaled-but-not-Started → continuity-checked replay-to-N only** (tip≠baseline + journals → `Corrupt`; damaged middle → `Corrupt`); never invent Started; never reclaim ids; **CREATE `.xgc` → GC `.x1` → clear Intent → unlink `.xgc`** after cleanup; clear-Started N/A.
     - Admitted NativeV2 / MigratedV2, no local Ack yet: verify watermark past Started ids; query by `request_id` — `Found` + breadcrumb/N+1 + **full baseline 4-tuple** bind → tip → flip → **full cleanup chain** (keep all Started names until `.clr` + PostSealFinalizing + `.xgc`-authorized Intent clear completes); `TransportUnavailable` → keep started, fence, re-query (do **not** PreSeal-abandon); `NotFound` → abandon only if tip still baseline **via ClrAbandoned** (`.abd` A0 first; with or without C; Intent AbandonFinalizing before unlink A; then CREATE `.xgc` → GC `.x1` → clear I → unlink `.xgc`). `LegacyStarted` / V-without-M → fence (companion migrate) — not this branch.
     - Migration mid-cuts (round-56): `L only` / `L+V.tmp` / `L+V` / `L+V+M.tmp` / `L+V+M` — never PreSeal, never new id reserve, never delete L; only `L+V+M` complete admits MigratedV2.
     - Clear mid-cuts (round-57/65/67/68/70): `C+L+V+M` / `C+L+V` / `C+L` / `C` / `I.PostSealFinalizing + names gone` / `I+.xgc + gapped .x1 + gates cleared` — resume cleanup / Mode B converge / CREATE `.xgc` → GC `.x1` → clear I → unlink `.xgc`; never PreSeal; never Corrupt solely for leftover names (when **no** `.xgc`), nearly-finished clear, **or** authorized mid-GC `.x1` gaps under Mode-B-eligible `.xgc`; **`I+.xgc + leftover Started/C` → Corrupt** (early `.xgc`; MUST NOT Mode B).
     - Seal Acked, tip not yet persisted, CURRENT still N: if N tip == bridge.prev_tip, re-persist tip → flip → **full cleanup chain**. Tip ≠ bridge after `SealExportStarted` → `Corrupt`.
     - Tip@N+1, CURRENT still N: flip iff N tip == bridge → **full cleanup chain** (do **not** clear Started after bare journal replay).
     - Both tip and CURRENT at N+1: resume **full cleanup chain** from remaining journal/tombstone/close/catch-up/GC/proof-valid `.clr`/PostSealFinalizing; clear Started **only** via `.clr` v2 + PostSealCommittedProof + Intent Finalizing; then normal.
     - Power-cut must-pass at every link of the cleanup chain (flip / apply / tombstone / intake-close / post-close catch-up / GC A / GC B / `.clr` / Intent PostSealFinalizing / ordered unlink / Intent clear).
   - Must-pass (round-35…57): prior cases **plus** `Path B before durable candidate_id → illegal`; `true-SPSC mask=1 close does not wait slots 1..7`; `close drain → new .sj1 → catch-up → GC A`; `GC A while applied_hw < commit_hw → illegal`; `Started topology ≠ recovery freeze → fence`; `§10.3 flip path uses full cleanup chain (no apply→close→GC shorthand)`; `legacy 192B Started → fence not empty-topology PostSeal`; `v2 Started 238B + kek_key_id in MAC (no try-all)`; `migrate closed L↔V + digests in M`; `.clr` mid-unlink never PreSeal/Corrupt`; `migrate companion: L kept + V CREATE_NEW + M bind → MigratedV2`; `power-cut at every migrate cut → never PreSeal / never new ids`; `V without M → LegacyStarted fence`; `delete/replace L to install v2 → non-compliant`; `hung producer → deadline → retain .jhw + fence + Started kept`; `timeout retry bumps epoch`; `watermark reserve → journal → crash before Started → replay-to-N only, ids not reused`; `journal Ack of >4096B → refuse`; `journal admit and apply share allowlist+size+schema+time provenance+authenticated baseline+MAC recompute`; `non-embeddable embedded_type → Corrupt`; `record_type != embedded_type → Corrupt`; `UnknownBootstrap journal → crash → clock published → replay still Unknown+0`; `UTC journal → clock loss → replay still original recorded_utc_ms`; `append_seal_journal_apply` has **no** separate `FrameTimeKind` parameter; `PostSeal journal baseline ≠ SealExportStarted/bridge → Corrupt`; `MAC-omitted baseline bind → non-compliant`; `Path B then Path A → illegal/Corrupt`; `unapplied journals + tip≠baseline → Corrupt`; `Started baseline ≠ id-reserve pin → refuse/Corrupt`; `Found without full baseline 4-tuple bind → non-compliant`; `NotFound abandon with tip≠baseline → Corrupt`; `old entry_mac + tampered payload → Corrupt, zero business effects`; `admit without entry_mac compute → non-compliant`; `FixedMetaBytes guessed / !=138 → non-compliant`; `store frame MAC alone authorizes apply → non-compliant`; `Acked journal seq=1,2 → delete/truncate seq=2 → restart → Corrupt, zero replay, zero CURRENT flip`; `final .sj1 length/MAC anomaly with seq≤commit_hw → Corrupt`; `only *.sj1.tmp may discard`; `Acked final → republish same seq different payload → Corrupt, original unchanged`; `publish → HW → crash before Ack → restart same seq → byte-equal Ack`; `final durable + prev_hw < seq → byte-equal still += both counters`; `HW already ≥ seq → byte-equal skips both`; `apply+tombstone+delete all → counters 0 → new candidate admits`; `crash after .jhw delete with residual .jts → resume GC → drain complete`; `drain-complete never requires live .jts`; `.jhw` regression → Corrupt`; `Windows CreateHardLinkW preferred → parent FlushFileBuffers → kill before .jhw → byte-equal +=`; `-= before unlink parent flush → non-compliant`; `every crash-window table row`; `last Applied → GC A + concurrent PostSeal handoff → .jhw retained or intake-closed`; `Started + missing .jhw → Path B fail-closed, never invent seq`; `Tip@N+1 → clear Started before drain-complete → non-compliant`; `late SPSC tail after naive empty → close protocol must prevent post-.jhw handoff`.

### 10.3 Anchor / `CURRENT` / generation-directory durability — fixes round-10/12 P1; Windows honesty round-23 P1; journal no-replace split round-50

Per-ACK tip-anchor and `CURRENT` pointer:

- **POSIX (power-loss target when volume honors fsync):** write `X.tmp` → `fsync(X.tmp)` → `rename(X.tmp, X)` → `fsync(parent directory fd)`.
- **Windows (round-23 P1 — honest contract, not a false POSIX equivalence):**
  - **Supported baseline:** NTFS on a local fixed volume. Network/redirected/ Removable/ReFS-without-flush guarantees are **out of scope**; treat as `IoError` / refuse durable mode if the store path is not NTFS-local.
  - **File replace (CURRENT / tip / `.jhw` / Started `.clr`/`.abd` / Intent phase-only):** create `X.tmp` → `FlushFileBuffers(tmp)` → `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`. **Forbidden** for seal-journal finals (`.sj1` / `.jts`) and Started finals L/V/M — those use §10.1 no-replace / hardlink / CREATE_NEW (migration = companion, never replace legacy). **`.clr` / `.abd` / `compaction-candidate-intent`** may REPLACE **only** to raise `phase` monotonically after Authorized/Building CREATE_NEW.
  - **Directory metadata:** open the parent directory with `CreateFileW(..., FILE_FLAG_BACKUP_SEMANTICS)` and `FlushFileBuffers(dirHandle)` after the replace (and after creating `gen-N+1/`, and after every journal hardlink/unlink/GC delete). `MOVEFILE_WRITE_THROUGH` alone is **not** claimed to equal POSIX `fsync(dirfd)`.
  - **What this claims:** crash-consistency under the above flush sequence on NTFS-local (process kill / clean shutdown). **What this does not claim until power-cut fault-inject passes:** full power-loss durability equivalent to POSIX dir-fsync. Until that matrix entry passes, Windows deployments must be documented as **fail-closed under detected flush failure**, not "power-loss protected."
  - **Failure determination:** any `FlushFileBuffers` / `MoveFileExW` failure → `IoError`, do not treat `CURRENT` as advanced; leave prior generation as authoritative.
- Torn-tmp recovery unchanged.

**New generation directory create order (round-12 P1; tip step added round-20 P1; source-tip pin round-34 P0; single-chain delegation round-55 P0):** before `CURRENT` may name generation `N+1`. This section is **durability / flush order only** — it does **not** define an alternate PreSeal/cleanup algorithm. Steps 4 and 6 **MUST** execute the §10.1 / §10.2 authoritative procedures verbatim (no abbreviated rewrite).

0. Refuse if `N == UINT32_MAX` (generation exhaustion — §10.1). Refuse if a prior **`CompactionCandidateIntent`** still exists (finish that candidate’s cleanup first).
1. Pin `CompactionSourceBaseline`; **`CREATE_NEW`+fsync `CompactionCandidateIntentWire` (140B; phase=Building)** binding baseline + `target_generation=N+1` + `build_nonce` (**before** any `gen-N+1/` byte). Then create directory `gen-N+1/` (or keyspace).
2. Write and flush `log`, `anchor`, `meta` (including the `GenerationBridge` frame with `prev_tip_* == baseline == Intent.baseline_*`) inside it. Re-verify tip == baseline after every yield during this step; mismatch → PreSeal-abandon (clean G + TipExportProducerResume if pause armed + CREATE `.xgc` (316B PreSealAbandonClear) → GC `.x1` if any → clear Intent → unlink `.xgc`) and restart.
3. Flush the `gen-N+1/` directory (POSIX `fsync` dirfd / Windows directory `FlushFileBuffers` above).
4. Re-verify tip == baseline; execute **§10.1 full PreSeal admission** through **durable id-reserve + Intent→Reserved → freeze handoff topology → `CREATE_NEW`+fsync packed `SealExportStartedWire` v2** (238B; MAC `HY-SEALSTART-v2` under **`kek_key_id`**; topology required; **refuse** if LegacyStarted / `.v2` / `.mig` / `.clr` / `.abd` present; require Intent Reserved) → **Intent→StartedPublished** → **direct** `export_and_wait_ack(GenerationSeal)` (§10.1 / §10.2). Abbreviated “reserve → Started without freeze/topology/`kek_key_id`/Intent” or “Started while residual `.abd`” or “G before Building Intent” is **non-compliant**.
5. Re-verify tip == baseline; durably persist `LastRemoteAckedTip` for `{new_generation, final_seq, final_tip_mac, key_id}` (§10.2 point 4 / §10.2.1). Failure → stop; do not flip. PostSeal mandatory inputs: path B only into reserved journal (§10.1). Discard residual gen-N outbox without send.
6. Flip `CURRENT` with the atomic-replace contract above, then flush `CURRENT`'s parent directory; execute the **§10.2 normative PostSeal cleanup chain** for `candidate_id` onto N+1: continuity-checked apply → tombstone → unlink → **intake-close protocol** → **post-close apply catch-up** → GC A → GC B → journal drain-complete → **`.clr` authorize → Intent→PostSealFinalizing → ordered unlink M→V→L → CAPTURE from C → unlink C → CREATE `.xgc` (316B; `HY-COMPINTENT-GC-v2`; DurableCleanupAuthEvidence) → TipExportProducerResume (idempotent) → GC `.x1` → clear Intent → unlink `.xgc` last** (§10.1 / §10.2). Abbreviated “apply/tombstone → intake-closed → GC” **without catch-up** or “unlink L/V/M without `.clr`” or “clear Intent while still StartedPublished after Started/C gone” or “ordered unlink → TipExport → CREATE” / “ordered unlink → clear Intent” **without** CAPTURE + `.xgc` tail or “GC `.x1` without prior `.xgc`” or “resume → clear Intent” / “TipExport → CREATE” **without** CREATE `.xgc` → TipExport (idempotent) → GC `.x1` → unlink `.xgc` last is **non-compliant**. Then live appends.

A crash that leaves durable `CURRENT` pointing at a missing `gen-N+1/` is thereby prevented under the claimed flush model: `CURRENT` is not flipped until the directory contents, the directory entry, the seal Ack, **and** the tip breadcrumb are durable under that model, **and** archived N tip equals `bridge.prev_tip` (incl. `prev_key_id`). Recovery that finds `CURRENT →` missing generation → `IoError`. Recovery that finds tip already at N+1 with CURRENT still at N completes step 6 only when N tip == bridge.prev_tip; then runs the **full cleanup chain** onto N+1 (not “drain journal” shorthand, not “ordered unlink → clear Intent” shorthand). Tip drift after `SealExportStarted` → `Corrupt`. Power-loss after remote seal persist before local Ack is recovered via **NativeV2Started** / **MigratedV2Started** + `query_seal_by_request_id` (§10.1) — not as PreSeal. Legacy 192B / incomplete companion migrate → fail-closed pending offline **companion+mig** procedure (§10 ABI) — never invent topology, never delete/replace L, never PreSeal-abandon as “missing.” `Unavailable` alone must not PreSeal-abandon (round-38). Admitted v2 MAC-ok breadcrumb is PostSeal even if parent-dir flush is undecidable (round-40). **Cross-consistency (round-55…69):** every `CURRENT` flip path — §10.1, §10.2, §10.3, L5 §6.1.3 — is the same chain: `freeze → Started(v2+kek_key_id) → … → tip → flip → apply → close → catch-up → GC A/B → journal drain-complete → PostSealCommittedProof → .clr authorize → Intent PostSealFinalizing → ordered unlink M→V→L → CAPTURE from C → unlink C → CREATE .xgc → TipExportProducerResume (idempotent) → GC .x1 → clear Intent → unlink .xgc last`. Authenticated-`NotFound` abandon (with or without unauthorized `.clr`) uses **ClrAbandoned** (`.abd` A0 first; Intent AbandonFinalizing while A present → CAPTURE from A → unlink A → CREATE `.xgc` → TipExportProducerResume (idempotent) → GC `.x1` → clear Intent → unlink `.xgc` last) — never leaves C occupying the name; never presents `… → unlink A → resume → clear Intent` without the `.xgc` segment as a complete path.

## Before implementation begins

1. This spec itself needs Architect acceptance (same process that produced the review this spec responds to).
2. `docs/SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` depends on every section here and must not be implemented first.
3. A real dry-run harness exercising §3/§4/§5/§6 against Binance **testnet** (owner-provided testnet credentials, never production) before any of this is trusted.
4. Independent review of the actual implementation once written — this repo's own governance for anything execution-adjacent (see `CLAUDE.md`'s boundary section) still applies regardless of the lighter engineering-ceremony rules for ordinary native work.
5. **Mandatory fault-injection matrix** (shared with L5) — must pass before any claim that P0s are closed in an implementation:
   - **时钟跨边界 header**: estimate within `error_bound` of a bucket boundary, weight header present, no trusted `Date` → `BucketIdentity::Unknown` hold; no wipe-on-rotate reopen (§7.1).
   - **快时钟 crash restart**: wall clock stepped +1h at restart after kill -9 in hot bucket → still zero packets for full **steady** wait; first packet is `/time` only after wait (§7.0 / §7.5.1).
   - **正常取消轮询**: `Accepted` + exchange `CANCELED` → `Cancelled` without false escalate; `Accepted`+`NEW` and `CancelRequested`+`PENDING_CANCEL` do not call `transition_to`; `CancelRequested`+`NEW`/`PARTIALLY_FILLED` reverse to Accepted/PartialFill (§6.6).
   - **anchor 不可达 override + degraded hard-lag**: sidecar admits with bound `LastRemoteAckedTip`; crash→restart→new-override **cannot** reset the 256-frame budget; inherited backlog over hard-lag → ReadOnlyDrain (zero new appends); empty-ring fresh-baseline is forbidden (§10.2 / §10.2.1).
   - **Phase-B / no-clock freeze kinds (round-21)**:
     - `429-before-clock-publish` → watermark + freeze Acked (`UnknownBootstrap`), `source=3` when Retry-After missing.
     - `418-before-clock-publish` → `source=2` + `UnknownBootstrap` (never demoted to `source=3`).
     - `429-long-Retry-After-before-clock-publish` → keep `source=0`, wait ≥ parsed multi-day via `conservative_wait_ms` (not shortened to IP-window alone).
   - **Single-episode / permanent not forgotten (round-22)**: `old-source2 + new-source0/3 + clear-new + restart` → `out_permanent_latch` still true; merge path never allocated a second epoch that hid the fence (§7.3.1).
   - **No UTC→unknown fail-open (round-23)**: long `ServerCorrectedUtc` then unknown merge + restart → still honors max UTC and max `conservative_wait_ms` (fold-all-frames) (§7.3.1).
   - **Dual-deadline WaitSatisfied (round-25/26)**: UTC+unknown merge → Arm Ack → full sink-measured wait → `FreezeWaitSatisfied` Ack → only then `ProbeVerified` with `/time` may clear; immediate WaitSatisfied after Arm rejected by sink elapsed check; `ProbeVerified` without WaitSatisfied rejected (§7.3.1 / §10).
   - **Equal-wait rematch / wait_generation (round-31)**: `unknown 429 wait=120 → WaitSatisfied → new unknown 429 wait=120 → old WaitSatisfied rejected → re-Arm → full wait → Satisfy` before probe/clear; compaction drops stale-generation Arm/Satisfy (§7.3.1 / §10).
   - **Legacy upgrade rematch (round-32/33)**: pre-`wait_generation` durable log `429 wait → Satisfy → 429 wait → upgrade/restart` → `out_has_wait_satisfied=false` (no gen=0→1 promote) → migration/compaction seals gen `≥1` → re-Arm → full wait → Satisfy before probe/clear; compaction must not retain legacy Satisfy as current-gen evidence when a later wait-bearing freeze exists; Arm/Satisfy/probe/clear with `wait_generation==0` while wait>0 are Rejected (§7.3.1 / §10 / L5 §6.1.3).
   - **Sequence-proven rewrite requires Arm after last wait-bearing (round-33)**: `freeze#1 → Arm → freeze#2 → short-wait Satisfy` must **not** rewrite; both Arm.seq and Satisfy.seq must be strictly after every wait-bearing freeze; otherwise drop → re-Arm → full wait (§7.3.1 / §10).
   - **Compaction snapshot ABI (round-33)**: uncleared epoch with folded `wait_generation > 1` compaction uses `append_compacted_freeze_snapshot` (no `prior+1`); live `append_rate_freeze` for the same payload Rejects; compacted wait evidence does not create `arm_ack_steady` (§10 / L5 §6.1.3).
   - **Compaction yield tip pin (round-34)**: retain-scan baseline tip → yield → live 429/OrderEvent appends on N → tip mismatch → abandon N+1 candidate → rescan; no event loss; stale Satisfy must not seal (§10.1 / §10.2 / L5 §6.1.3).
   - **Seal quiesce PreSeal/PostSeal (round-35…46)**: volatile buffer forbidden; busy/`Failed`-without-durable for received 429 illegal. **PreSeal** (no complete MAC-valid `SealExportStarted`): A or B after tip outbox drain + quiesce + **durable id reserve + baseline pin** + **B-sticky** drain + journal admit gates (incl. time provenance + authenticated baseline + **MUST compute entry_mac**). **PostSeal**: B only. Must-pass: prior round-45 cases **plus** `HY-SEALJRN-v1 recompute at every apply`; `packed SealJournalEntryWire FixedMeta=138`; `old MAC + tampered payload → Corrupt` (§10.1 / §10.2).
   - **Atomic seal-journal apply (round-40…57)**: only `append_seal_journal_apply(SealJournalAppliedView)`; **MUST recompute** `entry_mac`; continuity `1..commit_hw` before apply/flip; no-replace publish; tombstone+mask-frozen intake-close+deadline+**post-close catch-up**+GC+**`.clr` v2 clear**; clear Started only via proof-valid `.clr` after flip+drain; index `{candidate_id,journal_seq}` only; §10.3 delegates to this chain (§10.1 / §10.2 / §10.3).
   - **SealExportStartedWire v2 + companion migrate + `.clr`/`.abd` + Intent (round-55…72)**: packed LE 238B with `kek_key_id` (no try-all); legacy 192B → fail-closed + single `legacy_kek_key_id`; migrate via `.v2` + `.mig` v2 (full-file digests + closed L↔V equalities); clear via **`.clr` v2 (304B)** only when **PostSealCommittedProof** holds; wrongful/draft `.clr` → ClrUnauthorized / query-first; **ClrAbandoned `.abd` (192B)** on **every** authenticated-NotFound Started abandon + tip==baseline + empty journal (with-C or no-C; no-C: mask.C=0 + digest_C=0; never clear Started without A; never leave unauthorized C); greenfield Started refuses residual `.abd` until ResumeAuthorized + unlink A; GenGone (no-replace T; G+T missing after Started → Corrupt); ResumeAuthorized while `.abd` kept (producer paused) → `.x1` → Intent **AbandonFinalizing** → CAPTURE from A → unlink A → **CREATE `.xgc` → TipExportProducerResume (idempotent) → GC `.x1` → clear Intent → unlink `.xgc`**; **`CompactionCandidateIntent` 140B** before G (phase enum 0..4; wire size unchanged) + **`CompactionIntentTransitionWire` 176B** no-replace `.x1` before every Intent phase REPLACE + **`CompactionIntentGcAuthorizedWire` 316B** no-replace `.xgc` before any `.x1` unlink; Building|Reserved+no Started+no A → PreSeal-abandon (not Corrupt; CREATE `.xgc` → GC `.x1` if any → clear I → unlink `.xgc`); **StartedPublished+no Started+no A → Corrupt/IoError**; **PostSealFinalizing/AbandonFinalizing+no Started(+no A)+no early `.xgc` → CREATE `.xgc` → GC `.x1` → clear I → unlink `.xgc` (NOT Corrupt)**; early `.xgc` + leftover gates → Corrupt; no Intent+(G|T) → Corrupt; Path-A PreSeal-abandon still does not require `.abd`; `.clr` vs LegacyStarted → Corrupt; power-cut mid-clear/mid-abandon/mid-`.x1`-GC under Mode-B-eligible `.xgc` resumes (never PreSeal/Corrupt/producer-stall/use-after-unlink/false-Corrupt gap); never invent empty topology; §10.3 cross-path must not abbreviate away `.xgc` (§10 ABI / §10.1 / §10.3).
   - **§10.3 unified chain + `.xgc` tail (round-55…72 P0)**: every `CURRENT` flip = `freeze → Started(v2+kek) → … → tip → flip → apply → close → catch-up → GC A/B → journal drain → PostSealCommittedProof → .clr authorize → .x1 → Intent PostSealFinalizing → ordered unlink M→V→L → CAPTURE from C → unlink C → CREATE .xgc (316B DurableCleanupAuthEvidence) → TipExportProducerResume (idempotent) → GC .x1 → clear Intent → unlink .xgc last`; Abandon sibling = `… → ResumeAuthorized → .x1 → Intent AbandonFinalizing → CAPTURE from A → unlink A → CREATE .xgc → TipExportProducerResume (idempotent) → GC .x1 → clear Intent → unlink .xgc last`; Forbidden as complete terminal path: `ordered unlink → clear Intent`, `resume → clear I` / `resume → clear Intent` without `.xgc` segment; must-pass: `§10.3 flip → ordered unlink → crash at .xgc CREATE / each .x1 unlink / Intent unlink / .xgc unlink → Mode B converge when .xgc v2 valid AND Mode-B PhysicalCleanupPreconditions hold from DurableCleanupAuthEvidence (not Corrupt; early .xgc + leftover Started/C → Corrupt; C/A-gone mid-GC converges)` (§10.3 / §10.1 / L5 §6.1.3).
   - **CompactionCandidateIntent (round-64…72)**: `G fsync → crash (not yet reserve/Started) → restart → CREATE .xgc → clear I → unlink .xgc + retry` (not fence); `StartedPublished → Started/A abnormally missing → Corrupt`; `last .clr unlink / last Started gone → crash before Intent clear → I.PostSealFinalizing → converge via .xgc-authorized clear (not fence)`; `.abd unlink → crash before Intent clear → I.AbandonFinalizing → converge via .xgc-authorized clear (not fence)`; Intent tip-drift abandon; leftover Intent (incl. *Finalizing) / orphan `.x1` / leftover `.xgc` blocks new build; phase non-monotonic / Mode-A `.x1` break / PostSeal↔Abandon cross → Corrupt; clear Intent only after final cleanup + **CREATE `.xgc` → GC `.x1` → Intent → unlink `.xgc`** (`.clr`+PostSealFinalizing / ClrAbandoned A4+AbandonFinalizing / PreSeal-abandon) (§10 ABI / §10.1 / §10.2 / §10.3).
   - **Terminal Intent Finalizing (round-65…72)**: PostSeal: `.clr` Authorized → `.x1` → Intent PostSealFinalizing **before** any Started unlink → ordered unlink M→V→L → CAPTURE from C → unlink C → CREATE `.xgc` (316B DurableCleanupAuthEvidence) → TipExportProducerResume (idempotent) → GC `.x1` → clear Intent → unlink `.xgc`; Abandon: ResumeAuthorized → `.x1` → Intent AbandonFinalizing **while A present** → CAPTURE from A → unlink A → CREATE `.xgc` → TipExportProducerResume (idempotent) → GC `.x1` → clear Intent → unlink `.xgc`; recovery from either Finalizing finishes cleanup (NOT Corrupt) when **no** early `.xgc` with leftover gates; Mode B after C/A gone uses `.xgc` evidence (never live `.clr`/`.abd`); wire stays 140B / format_version=1; no `completion_kind`; path proof via **`CompactionIntentTransitionWire`** 176B + GC auth via **`.xgc` 316B** (§10 ABI / §10.1 / §10.2 / §10.3 / L5 §6.1.3).
   - **CompactionIntentTransitionWire path proof (round-66…70)**: every Intent phase raise persists no-replace `.x1` **before** Intent REPLACE; Mode A recovery verifies legal chain / one-step lag; illegal cross/jump + crash → Corrupt; power-cut after `.x1` before REPLACE → complete REPLACE; terminal clear CREATE `.xgc` → GC `.x1` → Intent → `.xgc` last; orphan `.x1` without Mode-B-eligible `.xgc` → Corrupt; Intent phase monotonic REPLACE alone does **not** prove no-cross (§10 ABI / §10.1 / L5 §6.1.3).
   - **CompactionIntentGcAuthorized mid-GC + early-`.xgc` fence (round-67…72 P0)**: durable no-replace `.xgc` 316B/`HY-COMPINTENT-GC-v2` with DurableCleanupAuthEvidence before any `.x1` unlink **only when CREATE-time PhysicalCleanupPreconditions hold**; Mode B converges authorized gaps **only after** Mode-B PhysicalCleanupPreconditions from `.xgc` evidence + live gate absence (r70/r71 — “valid `.xgc` ⇒ Mode B” WITHDRAWN; never re-read deleted `.clr`/`.abd`; TipExportProducerResume idempotent); `mid-GC delete x1[1] from {1,2,3} with I+.xgc + gates cleared → converge (not false Corrupt)`; `same gap without .xgc → Corrupt`; `Intent gone + Mode-B-eligible .xgc + gates cleared → finish .xgc (not Corrupt)`; `Intent gone + .xgc + leftover Started|C|A → Corrupt (not finish .xgc only)`; `PostSealFinalizing + early .xgc + leftover Started/C → Corrupt`; `AbandonFinalizing + early .xgc + leftover A → Corrupt`; `forged/dual/early .xgc / dual disposition mismatch → Corrupt`; `Reserved/PostSeal/Abandon .xgc with terminal_transition_mac=0 while .x1 remain → Corrupt`; `Building PreSeal .xgc with non-zero mac while no .x1 → Corrupt`; must-pass `C deleted → .xgc written → delete any x1 → crash` and `A deleted → .xgc written → delete any x1 → crash` → Mode B without live C/A; every Recovery/crash-window/must-pass/§10.3 finish path CREATE `.xgc` before clear I; r66 GC-lag without auth **withdrawn**; §10.3 must not reintroduce `ordered unlink → clear Intent` shorthand; live §10.1/§10.3/TipExport/Recovery/must-pass must not prescribe write of v1/204B / `HY-COMPINTENT-GC-v1` or `TipExport → CREATE` / `unlink A → resume → CREATE` (r72 residual — recovery fail-closes legacy → self-lock) (§10 ABI / §10.1 / §10.3 / L5 §6.1.3).
   - **`.clr` PostSeal bind (round-58)**: `Started + zero journal + wrongful `.clr` + remote Found → keep Started → query Found → tip/flip/drain → then authorize C (no generation fork) (§10 ABI / §10.1).
   - **Seal-journal wire layout (round-46)**: packed LE `SealJournalEntryWire`; `kSealJournalFixedMetaBytes=138`; soft-cap = slots **and** live-`.sj1` owner counters (no per-admit readdir); `static_assert`/encode-length tests required (§10.1).
   - **Journal continuity / commit HW (round-47)**: `SealJournalCommitWatermark`; final `.sj1` damage with `seq≤commit_hw` → `Corrupt`; `Acked 1,2 → delete/truncate 2 → Corrupt`; only `*.sj1.tmp` discardable; temp→no-replace-publish→watermark→Ack (§10.1).
   - **No-replace publish + tombstone GC (round-48)**: `renameat2(RENAME_NOREPLACE)` / Windows non-replace; byte-equal idempotent republish; `SealJournalTombstoneWire` (`kSealJournalTombstoneBytes=108`) binds `entry_mac`; GC phase A then B; `.jhw` monotonic (§10.1).
   - **Drain-complete / counter idempotency (round-49)**: end state = no `.sj1`/`.jhw`/`.jts`; `+=` both counters iff created_final ∨ (`prev_hw < seq`); partial-GC resume; dual `.jts`+`.sj1` → verify then unlink (§10.1).
   - **Windows journal IO + crash-window table (round-50)**: preferred `CreateHardLinkW`+parent flush; CREATE_NEW copy fallback only; `-=` only after unlink parent flush; preallocated ≤`kSealJournalMaxEntryBytes`; every crash-window table cut; power-cut honesty same as §10.3 (§10.1 / §10.3).
   - **Intake-closed before GC A (round-51…57)**: frozen `registered_producer_mask` + `producer_count` + `ring_id[]` before PostSeal **and MAC-bound into `SealExportStartedWire` v2** with **`kek_key_id`**; close waits **mask-only**; `close_deadline_steady` + max poll; timeout → retain `.jhw` + fence + no GC/clear Started; retry bumps epoch; **post-close apply catch-up** before GC A; `Started`+missing `.jhw` → Path B fail-closed; recovery freeze **must equal** Started topology; clear Started only via **proof-valid `.clr` v2** after flip+journal drain; §10.3 must not skip catch-up (§10.1 / §10.2 / §10.3).
   - **Seal-journal time provenance (round-43)**: `UnknownBootstrap journal → crash → clock published → replay Unknown+0`; `UTC journal → clock loss → replay original UTC` (§10.1 / L5 §6.1.1).
   - **Journal baseline tip bind (round-44/45)**: baseline 4-tuple in `entry_mac`; PostSeal == Started/bridge; PreSeal-abandon intra-candidate + B-sticky Corrupt if tip≠baseline with journals (§10.1).
   - **B-sticky PreSeal (round-45)**: Path A illegal after any journal Ack for `candidate_id`; `Path B freeze → Path A freeze → store order matches observation order` (§10.1).
   - **Watermark-before-Started (round-41/42)**: durable watermark reserve precedes Path B and Started; Started reuses reserved ids without second advance; recovery verifies `next_* > started.ids` (§10.1).
   - **Outbox drain before seal (round-40)**: PreSeal empties `ExportOutboxRing`; after tip@N+1 no gen-N re-send (§10.2.1).
   - **Bridge prev_key_id + evidence key_id (round-35/36)**: compaction/recovery bind `prev_key_id`; `CompactedFreezeWaitEvidencePayload.source_baseline_key_id` required; omit/mismatch → abandon/`Corrupt` (§10 / §10.2).
   - **Generation exhaustion (round-34)**: `CURRENT == UINT32_MAX` → compaction refused; fence; no wrap (§10.1).
   - **CompactedFreezeWaitEvidence single payload (round-34)**: one frame / one payload layout; dual Arm+Satisfy compaction substitute Rejected; content-root MAC covers that payload (§10).
   - **Deadline-only merge keeps wait_generation (round-31)**: after current-gen WaitSatisfied, UTC-only merge (incoming wait contribution `0`) copy-forwards gen; existing WaitSatisfied remains valid for probe/clear (§7.3.1).
   - **Failed clear leaves WaitSatisfied (round-31)**: `append_freeze_clear` Rejected must not invalidate current-gen WaitSatisfied; retry clear without re-wait (§10).
   - **Probe Ack-before-send (round-26)**: `try_reserve` → durable `FreezeProbeAttempt` Ack → only then `/time`; kill after Ack burns ordinal; kill before Ack allows retry without budget burn; nine reserved-but-unacked loops cannot inflate network beyond 8 Acked attempts (§7.3.1).
   - **Probe deadline durable schedule (round-27/28/29)**: multi-day `deadline_utc_ms` → attempt `not_before = max(backoff, deadline)` → crash mid-ban → restore folds deadline → no early `/time` storm; missing UTC uses **ClockRepublishOrVerify** (burns ordinal; **may ProbeVerified-clear** if `serverTime >= deadline`) (§7.3.1 / §7.1).
   - **No freeze TimeResync bypass (round-28)**: kill -9 loop during active freeze cannot obtain unbounded `/time` via once-per-boot TimeResyncCredit; only durable 8-cap FreezeProbeAttempt (§7.1 / §7.3.1).
   - **ClockRepublishOrVerify last-ordinal clear (round-29)**: `attempts=7 → crash loses clock → ordinal 8 → serverTime >= deadline (+ WaitSatisfied if required) → FreezeClear Ack → restart not frozen`; wait>0 refuses probe until WaitSatisfied (§7.3.1).
   - **Probe attempt sink checks (round-27)**: wrong/duplicate ordinal, wrong epoch, or `not_before < deadline` → append Failed; confirm binds Ack sequence (§10).
   - **Probe credit restore + backoff (round-25/26/27)**: durable attempts at 7 → kill -9 → restart → `restore_from_recovery` → backoff **and** deadline re-applied → 8th attempt only after not_before; crash loop cannot reset to 0 or skip multi-day gate (§7.3.1).
   - **Epoch bind-only (round-26)**: foreign epoch to `try_reserve_probe` refuses; only `bind_new_episode_after_durable_create` after watermark+freeze Ack resets attempts (§7.3.1).
   - **Proof host ABI (round-26)**: `FreezeTimeProbeProof.tls_verified_host` required; MAC rebuild from payload; host mismatch rejected (§10).
   - **Clock seq / arithmetic (round-25)**: `publish` at `UINT32_MAX` refuses; pessimistic/signing overflow → Unknown/hold / refuse sign (§7.1.2).
   - **Typed clear + /time proof (round-22/23)**: forged/expired `ProbeVerified` without valid `FreezeTimeProbeProof` rejected; compaction+restart cannot drop freeze; probe/wait cannot clear `source=2`; only `OperatorAuthorized` + KEK-MAC lifts permanent.
   - **Probe budget (round-23/26)**: nine successful `/time` replies that do not clear → ordinals exhausted at 8, no reset; alarm (§7.3.1).
   - **Representability (round-23)**: UTC-or-steady overflow → permanent fence (§7.3).
   - **Windows flush (round-23)**: directory `FlushFileBuffers` failure aborts `CURRENT` flip; no claim of POSIX power-loss equivalence without power-cut inject (§10.3).
   - **Cleared freeze not re-armed**: valid `FreezeClear` ⇒ epoch not active; compaction may drop resolved history; restart does not wait `conservative_wait_ms` again (§7.3.1 / L5 §6.1.3).
   - **GenerationSeal tip before CURRENT**: seal Ack updates `LastRemoteAckedTip` to `{N+1, final_seq, …}` before flip; tip-persist failure aborts flip; crash after tip/before flip → recovery completes flip without false full-gen backlog (§10.2 / §10.3).
   - **Tip non-regression**: after seal tip at gen N+1, a late gen-N export Ack must not overwrite tip backward (monotonic CAS / single-writer drain) (§10.2.1).
   - **跨 generation rollback**: external seal at gen N+1 vs local CURRENT at older gen without bridge → `Corrupt`; legal compaction with bridge + external ACK before CURRENT → `Recovered`; crash after seal ACK before CURRENT flip → recovery completes the flip (§10.2 / L5 §6.1.3).
   - Immutable clock publish under concurrent resync — no data race / no early rotation (§7.1.2).
   - 418 missing Retry-After → permanent fence; 429 missing → IP-rollover wait, not 60s (§7.3).
   - `crash-after-GET` poll failures; ledger restart; outcome-audit fence; `-1021` zero re-POST; TLS/DNS matrix (prior rounds).
   An implementation that compiles but has not demonstrated these is not "P0-closed."
