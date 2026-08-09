# formal/ — TLA+ crash-recovery models

Layer 3 of `docs/SPEC_INVARIANTS.md`'s mechanization plan: mechanically verify
crash-safety properties of the durable-log design in
`docs/SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` / `docs/BINANCE_PRIVATE_REST_L4_SPEC.md`
with TLC (exhaustive model checking) instead of relying on another round of human/LLM
review to notice a contradiction.

Three independent models live here: `durable_log_recovery.tla` (the OrderEvent/Gate 8-9
crash-safety boundary), `freeze_episode_recovery.tla` (the L4 §10 freeze-episode
probe/Arm/WaitSatisfied/Clear state machine, further down this file), and
`inflight_lifecycle.tla` (the InFlightRegistry slot lifecycle, added by the full-repo
audit — see its own section at the bottom). Each has its own config table and MUST be
run separately. Two further models closed the audit's remaining named gaps:
`key_rotation.tla` (the two-step key rotation crash protocol, KEY-ROTATE-008) and
`depth_snapshot_bootstrap.tla` (the Binance depth snapshot bootstrap state machine,
MD-BOOK-002) — see their sections below.

## Setup (one-time)

TLC needs Java (already available locally — `java -version`). Fetch the official
TLA+ tools jar (pinned; not vendored in git — see `.gitignore`):

```bash
mkdir -p formal/tools
curl -sL -o formal/tools/tla2tools.jar https://github.com/tlaplus/tlaplus/releases/download/v1.7.4/tla2tools.jar
```

## The configs, and what each one MUST report

Run every one of these, not just the first. Three of them are *expected to fail* —
they are the model's own self-tests, and a run where they stop failing is a run where
the model has silently stopped proving anything.

| Config | Expected result |
|---|---|
| `durable_log_recovery.cfg` | **No error found** — the current (revision 73) design |
| `durable_log_recovery_buggy.cfg` | **`NoDoubleSend` violated** — regression control #1 |
| `durable_log_recovery_abortedpresend.cfg` | **`NoDoubleSend` violated** — regression control #2 |
| `durable_log_recovery_probe.cfg` | **`LateFsyncUnreachable` violated** — reachability probe |
| `durable_log_recovery_liveness.cfg` | **No error found** — `EventualResolution` holds |
| `durable_log_recovery_saturation.cfg` | **No error found** — bound-saturation check, manual only |

TLC exits `0` on success and `12` on an invariant violation, so CI inverts the check
for the three expected-to-fail configs (see `.github/workflows/ci-spec-verification.yml`).

```bash
cd formal
java -cp tools/tla2tools.jar tlc2.TLC -config durable_log_recovery.cfg durable_log_recovery.tla
```

### Why three configs must fail

- **`_buggy`** reintroduces the pre-revision-14 recovery bug (a Prepared-only durable
  frame recovers as `"Submitting"` instead of `"Ambiguous"`), which permits a second
  `Send` after crash+recover.
- **`_abortedpresend`** reintroduces revision 13's `AbortedPreSend` compensation —
  treating a `Failed` durable append as *proof* the operation did not happen. This is
  the config that makes the writer fence **load-bearing**: with the fence in place
  nothing else in the model discriminates it, so without this control the fence would
  be unverified decoration. Note it needs no crash at all.
- **`_probe`** asserts `LateFsyncUnreachable`, a claim we *want* refuted.
  `state[c] = "Intent"` coexisting with a durable `Prepared` frame is the unique
  signature of "the bytes landed but the caller was told Failed". If TLC cannot
  violate this, the late-fsync split is never exercised and every "no error" result
  from the main config is worth correspondingly less.

## Recorded results (2026-07-26, TLC 2.19, Java 25)

| Config | States (distinct) | Depth | Result |
|---|---|---|---|
| main | 239,633 | 39 | no error |
| buggy | 353 | — | `NoDoubleSend` violated ✓ |
| abortedpresend | 1,587 | — | `NoDoubleSend` violated ✓ |
| probe | 5 | — | `LateFsyncUnreachable` violated ✓ |
| liveness | 62,256 | 33 | no error (`EventualResolution` holds) |
| saturation | 527,876 | 40 | no error |

**Saturation check** (guards against a too-tight bound giving false confidence):
main at `MaxLogLen=9 / MaxCrashes=3 / MaxAppendFailures=2` → 239,633 states;
saturation at `MaxLogLen=12 / MaxCrashes=4 / MaxAppendFailures=3` → 527,876 states.
State count grew 2.2×, violations stayed at zero — so the smaller bound is not
masking anything. Re-run this pair whenever the model changes.

**Action coverage** (`-coverage 1` on the main config) — every action is live, in
particular the dangerous branches:

```
LandForWaitingCaller   28134 states     LandAfterAbandon (late fsync)   8280 states
AckAppend              21169 states     LoseAfterAbandon               8098 states
FailAppend             31290 states     OperatorRepairRestart          2267 states
Crash                  19916 states
```

## Scope

`durable_log_recovery.tla` models the Gate 8/9 `OrderSubmitPrepared` crash-safety
boundary (spec revision 14, round 13) — the single most important safety property in
either spec: **the real network POST is invoked at most once per client-order-id,
ever, across any number of process crashes** — plus the durable-append failure
semantics that make that property non-trivial.

The append is deliberately **not atomic**. `AckAppend` requires the bytes to have
landed (storage is honest about success) while `FailAppend` does *not* require that
they did not land (storage lies about failure — the late fsync). That asymmetry is
the spec's revision-14 P0 finding, and modeling it is what distinguishes this model
from the first revision, where the append was one atomic step and the whole crash
window was inexpressible.

It deliberately does **not** model compaction, GC, seal/journal, or the
freeze/rate-limit subsystem (spec rounds 30–72). This work only has changelog-level
grounding on that part of the design, not full prose+pseudocode depth — modeling it
without that would produce something that looks rigorous while encoding guesses,
which is worse than not modeling it at all. Extend the model once that depth is
actually read and ported into `native/include/hengyuan/durable_control_plane.hpp`
(Layer 2), the same discipline Layers 1 and 2 already follow.

**Known resolution limit:** because the writer is serialized on `WriterIdle`, a
late-landing frame is always appended before any subsequent frame. The sharper hazard
§6.1.1.1 describes — a late write occupying a sequence number a later frame already
used — needs the sequence-number/hash-chain layer and is out of this model's
resolution. What *is* covered is a late land racing `recovery_scan()`.

## Traps to not walk back into

These are load-bearing and non-obvious; each one produced a wrong result during
development before being fixed.

1. **The liveness config has no `CONSTRAINT` line. Do not add one.** TLC's liveness
   checker runs on the *constrained* state graph, so a state whose successors were
   pruned by a constraint has no outgoing edges and TLC treats stuttering there as a
   legal infinite behavior — reporting `EventualResolution` as violated with a trace
   that looks exactly like a real deadlock. Finiteness there comes from action guards
   (`MaxCrashes`, `MaxAppendFailures`, `OutcomeLatched`) instead, which is why those
   budgets are modeled as guarded variables rather than constraint clauses.
2. **`CHECK_DEADLOCK FALSE` is required in every config.** `Crash` is budget-guarded,
   so legitimate terminal states exist and would otherwise be reported as deadlocks.
   Nothing is lost — `EventualResolution` is strictly stronger than deadlock-freedom.
3. **`sendCount <= 3` in `StateConstraint` must not be tightened to 1** — that would
   make `NoDoubleSend` unfalsifiable by construction and every run would pass vacuously.
4. **Fairness is declared per-COID** (`\A c : WF_vars(A(c))`), never
   `WF_vars(\E c : A(c))` — the latter only promises that *some* COID progresses.
5. **`WF_vars(LandAfterAbandon \/ LoseAfterAbandon)` is on the disjunction**, not on
   each disjunct. On each separately it would force one of the two outcomes and make
   the other unreachable; on neither, an abandoned write pins `WriterIdle` false
   forever and deadlocks the model.
6. **No fairness on `Crash`, `FailAppend`, or `NetworkDeliver`** — we never assume a
   crash happens, that storage fails, or that a packet arrives.
7. **`pendingAppend` is a record with an `active` flag, not a record-or-string union.**
   TLC refuses to fingerprint a state where a string sentinel is compared against a
   record. Clearing the writer assigns the canonical `IdleWriter` value rather than
   flipping `active` in place, so an idle writer has one representation instead of
   many stale-field variants.

## Files

- `durable_log_recovery.tla` — the model (plain TLA+, not PlusCal — compact enough
  that the extra translation step isn't worth it at this scope).
- `durable_log_recovery*.cfg` — see the table above.
- `tools/tla2tools.jar` — gitignored; fetch with the command at the top.

---

# `freeze_episode_recovery.tla` — L4 §10 freeze-episode slice (轨道 C)

A second, **separate** model — not an extension of `durable_log_recovery.tla`, which
explicitly disclaims the freeze/rate-limit subsystem in its own header (see this
file's earlier section). Grounding for just the freeze-episode slice now exists
(`docs/BINANCE_PRIVATE_REST_L4_SPEC.md:2570-2816`), so this model covers exactly that
slice: the probe → Arm → WaitSatisfied → Clear state machine backing
`docs/SPEC_INVARIANTS.md`'s three already-documented freeze-subsystem invariants
(`FreezeProbeCredit`, `wait_ok`, `wait_generation`). It does **not** cover the much
larger compaction/generation-switch/seal-journal machinery in the rest of §10.

## The configs, and what each one MUST report

| Config | Expected result |
|---|---|
| `freeze_episode_recovery.cfg` | **No error found** — the current (correct) design |
| `freeze_episode_recovery_wait_ok_bug.cfg` | **`NoPrematureConservativeClear` violated** — regression control #1 (round 30) |
| `freeze_episode_recovery_generation_bug.cfg` | **`WaitSatisfiedOnlyForCurrentGeneration` violated** — regression control #2 (round 31/32) |
| `freeze_episode_recovery_probe.cfg` | **`AbandonedArmUnreachable` violated** — reachability probe |
| `freeze_episode_recovery_liveness.cfg` | **No error found** — `EventualFreezeResolution` holds |
| `freeze_episode_recovery_saturation.cfg` | **No error found** — bound-saturation check, manual only |

```bash
cd formal
java -cp tools/tla2tools.jar tlc2.TLC -config freeze_episode_recovery.cfg freeze_episode_recovery.tla
```

### Why three configs must fail

- **`_wait_ok_bug`** reintroduces round 30's bug: the sink Acks a `WaitSatisfied`
  frame without independently verifying real elapsed wait time, trusting whatever the
  caller's frame claims instead. `SinkVerifiesWaitIndependently = FALSE` drops the
  `AckAppend` guard that would otherwise refuse to Ack a forged claim.
- **`_generation_bug`** reintroduces round 31/32's bug: a `ConservativeWaitCompleted`
  clear accepts a `WaitSatisfied` bound to any generation `<=` the current one, not
  just an exact match — `PromoteLegacyGenerationCorrectly = FALSE` loosens the
  generation check that `AckAppend`/`LandAfterAbandon` re-verify at durability time
  (deliberately re-verified there, not at request time — see the model's own comment
  on `ClearViaConservativeWaitCompleted` for why a request-time-only check would be
  racy against `BumpGeneration`).
- **`_probe`** asserts `AbandonedArmUnreachable`, a claim we *want* refuted:
  `armed.active` alongside `~armedThisSession` and `~satisfied.active` is the unique
  signature of "this session Armed, then crashed before ever claiming
  `WaitSatisfied`" (spec:2745's "abandoned across process restart" rule). If TLC
  cannot violate this, that rule is never actually exercised.

## Recorded results (2026-08-01, TLC 2.19)

| Config | States (distinct) | Depth | Result |
|---|---|---|---|
| main | 4,995,065 | 61 | no error |
| wait_ok_bug | 588 | — | `NoPrematureConservativeClear` violated ✓ |
| generation_bug | 2,393 | — | `WaitSatisfiedOnlyForCurrentGeneration` violated ✓ |
| probe | 31 | — | `AbandonedArmUnreachable` violated ✓ |
| liveness | 70,835 | 32 | no error (`EventualFreezeResolution` holds) |
| saturation | 6,978,870 | 61 | no error |

**Saturation check**: main at `MaxCrashes=2` (others held at
`Fails=2/Probes=9/Episodes=3`) → 4,995,065 states; saturation at `MaxCrashes=3` (same
others) → 6,978,870 states. State count grew 1.4×, violations stayed at zero. Unlike
`durable_log_recovery.tla`'s saturation check (which bumps 2 independent budgets
together), this one bumps only `MaxCrashes` — an earlier attempt bumping all 4
independent budgets simultaneously (`Crashes=3/Fails=3/Probes=10/Episodes=4`) was
still growing past 17M states after 4 minutes with no end in sight, since this
model's 4 independent budgets multiply combinatorially in a way
`durable_log_recovery.tla`'s 2 do not.

## Design notes specific to this model

- **No growing append-only log.** Unlike `durable_log_recovery.tla`'s `durableLog`
  sequence, this model tracks only the LATEST record of each frame kind as scalar
  durable state (`armed`, `satisfied`, `clearKind`) — matching what
  `recover_control_plane()` actually reconstructs (latest-per-kind, not a full
  replay). One consequence: an abandoned write's durable effect applies directly at
  land time (`DurableEffect`), not via a separate log-scanning `Recover()` action —
  there is no log to scan.
- **A cleared episode's evidence is not wiped by the clear itself** — only
  `DetectFreeze` (starting the next episode) resets `armed`/`satisfied`/
  `probeAttempts`. This is what lets `NoPrematureConservativeClear` and
  `WaitSatisfiedOnlyForCurrentGeneration` check what justified a clear *after* it
  happened, without the check racing its own side effect.
- **Known resolution limit**: `ClearViaProbeVerified`'s actual ground-truth condition
  (serverTime genuinely past deadline) is not modeled — the guard only requires a
  genuine attempt was made. Neither regression target involves that path, so a full
  ghost-truth treatment would add complexity without discriminating power for a real
  historical bug.

## Files

- `freeze_episode_recovery.tla` — the model.
- `freeze_episode_recovery*.cfg` — see the table above.

---

## `inflight_lifecycle.tla` — InFlightRegistry slot lifecycle

Added by the full-repository audit (finding FORMAL-GAP-017). The argument for it is
empirical rather than aesthetic: that audit found real defects in three subsystems —
the InFlightRegistry lifecycle, two-step key rotation, and depth-snapshot bootstrap —
and **all three were on this file's own "deliberately not modeled" list**, while the
two subsystems that DO have models came through the same review with no findings. The
shortfall in this repo's formal coverage was breadth, not depth.

This model closes the highest-value of the three.

### What it models

`orchestrate_submit()` → `ToReconcileRing` → `poll_once()` → `ReconcileEventRing` →
`drain_reconcile_events()` → slot release. Deliberately abstract about everything else:
no durable log, no crash/recovery, no key material, no wall-clock or backoff. Those
either have their own model or are outside this property's resolution, and modeling
them here without that depth would produce something that looks rigorous while encoding
guesses — the same rule this file's Scope section already states for
`durable_log_recovery.tla`.

### The defect it discriminates

EXEC-INFLIGHT-003. An order the exchange **Accepted** kept its InFlightRegistry slot —
correctly, since it is resting on the book and a blind resubmit must stay blocked — but
was never handed to anything that could later discover it had filled or been cancelled.
`drain_reconcile_events()` releases a slot only on `is_exchange_final()`, so the slot
became unreleasable for the life of the process; 64 of them fail-closed every
subsequent submit. That violates the invariant the whole gate chain rests on: an
accepted request must eventually reach a terminal state.

### The configs, and what each one MUST report

| Config | Expected result |
|---|---|
| `inflight_lifecycle.cfg` | **No error found** — the current design |
| `inflight_lifecycle_untracked_bug.cfg` | **`NoLiveOrderIsUntracked` violated** — regression control |
| `inflight_lifecycle_liveness.cfg` | **No error found** — `EventualSlotRelease` holds |
| `inflight_lifecycle_saturation.cfg` | **No error found** — bound-saturation check, manual only |

```bash
cd formal
java -cp tools/tla2tools.jar tlc2.TLC -config inflight_lifecycle.cfg inflight_lifecycle.tla
```

**Why the control must fail.** `TrackAcceptedOrders = FALSE` reinstates the pre-audit
behaviour: `SubmitAccepted` takes a slot without pushing to `ToReconcileRing`. Since
`poll_once()` only ever sees what arrives on that ring, the order is stranded
immediately. If this config ever stops failing, the model has stopped discriminating
the defect it was written for and every clean run of the main config is worth
correspondingly less — same inversion discipline as `durable_log_recovery_buggy.cfg`
and the TSan negative controls.

### Recorded results (2026-08-08, TLC 2.19, Java 25)

| Config | States (distinct) | Depth | Result |
|---|---|---|---|
| main | 286,636 | 19 | no error |
| untracked_bug | — | 2 | `NoLiveOrderIsUntracked` violated ✓ |
| liveness | 12,825 | 15 | no error (`EventualSlotRelease` holds) |
| saturation | 29,110,403 | 28 | no error |

The regression control's counterexample is **two states deep** (Init → `SubmitAccepted`)
— the defect needed no crash, no concurrency and no unusual ordering to reach, which is
consistent with the runtime repro that found it.

**Saturation check**: main at `COIDs=3 / MaxSlots=2 / MaxQueryAttempts=2` → 286,636
states; saturation at `COIDs=4 / MaxQueryAttempts=3` → 29,110,403 states. A 101× growth
with violations still at zero, so the smaller bound is not masking anything. Note that
`MaxSlots` is deliberately NOT also raised: `COIDs=4 / MaxSlots=3 / MaxQueryAttempts=3`
exceeds 50M distinct states and buys no additional discrimination, given the
counterexample above is 2 states deep. Re-run this pair whenever the model changes.

### Still not modeled

The full-repository audit's three findings (EXEC-INFLIGHT-003, KEY-ROTATE-008,
MD-BOOK-002) are now all closed: `inflight_lifecycle.tla` above covers the first,
`key_rotation.tla` and `depth_snapshot_bootstrap.tla` below cover the other two. Each
was fixed in production code with unit-level regression tests first; the models exist
to exhaustively check the crash/interleaving windows those tests reach only by
construction.

---

## `key_rotation.tla` — two-step key rotation, crash safety (audit KEY-ROTATE-008)

The first of the two models that close this file's former "Still not modeled" list.
`rotate_active_key()` is a two-step DURABLE protocol
(`native/include/hengyuan/durable_audit_sink.hpp:502-551`): (1) append a `KeyRotated`
frame to the SIDECAR log plus the sidecar's own tip anchor — two separate fsyncs, each
independently crash-able; (2) re-anchor the MAIN log's tip under the new key — legally
skipped when the main log is empty. Only after both steps are durable does
`active_key_id_` flip. A crash between the two steps leaves the sidecar saying
"rotated to N" while the main tip anchor is still signed under the old key.

Recovery (`finalize_scan_with_anchor_check()`, lines 790-862) accepts that mismatch and
completes step 2 ONLY when all three of these hold — and only then:

- (a) the restart is configured with the NEW key,
- (b) the sidecar's last record proves old→new (note: the sidecar FRAME scan is what
  establishes `last_rotation_`, line 934; a torn sidecar anchor only sets
  `rotation_fenced_`, which gates future rotations, not the repair),
- (c) the main anchor is still signed by the OLD key.

Any other mismatch is Corrupt: the sink fences permanently and nothing is auto-repaired
(lines 843-845). The review checklist's parenthetical (that a torn sidecar anchor must
also force Corrupt) does NOT match the shipped code: the frame alone establishes the
proof the repair consults, so the model follows the code — a torn sidecar anchor plus
the (a)(b)(c) triple still repairs. The model's regression control is calibrated to the
code's actual rule.

`key_ring.hpp`'s retention lifecycle (audit KEY-RETIRE-009) is modeled as a separate
boot-level fact (`ring_keys`): a key that ever signed retained content must still be
loadable at recovery, or the scan fails closed — "the active key flipped" is not "the
old key is retired".

### The configs, and what each one MUST report

| Config | Expected result |
|---|---|
| `key_rotation.cfg` | **No error found** — the current design |
| `key_rotation_onestep_bug.cfg` | **`RepairOnlyOnProvenMismatch` violated** — regression control |
| `key_rotation_liveness.cfg` | **No error found** — `EventualConsistencyOrFenced` holds |

```bash
cd formal
java -cp tools/tla2tools.jar tlc2.TLC -config key_rotation.cfg key_rotation.tla
```

**Why the control must fail.** `RepairOnlyWhenProven = FALSE` reinstates the simpler
(but wrong) protocol the review explicitly warned against: recovery decides whether to
re-anchor from the main anchor's key_id alone — mismatch vs the configured key ⇒
rewrite — ignoring the sidecar proof entirely. That turns an operator
misconfiguration (restart configured with the new key while no rotation ever happened,
or a stale config after a completed one) into a silently "repaired" log instead of the
required fail-closed Corrupt + fence — exactly the unauthorized-identity-swap class of
confusion the sidecar consultation exists to prevent. If it ever stops failing, the
model has stopped discriminating the protection KEY-ROTATE-008 added, and every clean
run of the main config is worth correspondingly less — same inversion discipline as
`durable_log_recovery_buggy.cfg` and the TSan negative controls.

**Crash cuts explored.** The `Crash` action fires from every Running state, so TLC
reaches every cut exhaustively, including: before step 1 (nothing durable); between the
sidecar frame and the sidecar anchor (torn sidecar); between step 1 and step 2 (the
canonical interrupted rotation, repaired); inside step 2's anchor write (torn main
anchor, `"none"` → fail closed); after step 2 (rotation complete); and around an
empty-log rotation (step 2 legally skipped — recovery is Clean, never a repair).
Combined with the boot's nondeterministic choice of configured key and ring contents,
every (config, durable-state) mismatch combo is reached.

**Invariant-to-code map.**

| Invariant | Real code it pins |
|---|---|
| `RepairOnlyOnProvenMismatch` | the (a)(b)(c) gate at `durable_audit_sink.hpp:843-847`; negative controls `AnchorKeyMismatchNotProvenBySidecarIsStillCorrupt` / `NoRotationMeansAnchorKeyMismatchIsCorrupt` |
| `NoDataUnverifiableAfterRotation` | KEY-RETIRE-009's fail-closed lookups at lines 670/748/812 (`key_ring.hpp` `retire()`/`active_key()`); tests `ObservedKeyIdsReportsEveryKeyThatSignedTheLog` / `RetiringAnObservedKeyIsWhatBreaksRecovery` |
| `IoFailureFencesRotationOrSink` | the self-fencing failure branches at lines 519-523 / 528-532 / 541-545 |
| `FlipOnlyAfterStepsDurable` | the durable-before-flip ordering documented at lines 480-496 |
| `AnchorImpliesNonEmptyLog` | structural: the anchor is only ever written behind a frame |
| `EventualConsistencyOrFenced` (liveness) | recovery always lands in repair-consistent or fail-closed-fenced (lines 796-862) |

**The liveness fairness assumption, and why it is honest.** "A crash between the two
steps eventually restores a consistent state" is only checkable under (1) a bounded
crash budget (`MaxCrashes`, an action guard — never a `CONSTRAINT`: a pruned successor
set makes TLC's liveness checker treat stuttering as a legal infinite behavior, the
exact trap `durable_log_recovery_liveness.cfg`'s comment documents) and (2)
`WF_vars(RecoveryScan)`. That fairness corresponds to a real operating fact: a crashed
process is restarted by the supervisor, and `DurableAuditSink`'s constructor
unconditionally runs both recovery scans (`durable_audit_sink.hpp:320-331`), completing
an interrupted rotation's step 2 inside that same constructor (lines 746-754). The
model assumes the re-anchor I/O eventually succeeds; a transient failure returns
IoError and fences, and the next restart's scan re-attempts it — the same supervisor
loop.

### Recorded results (2026-08-09, TLC 2.19, Java 25)

| Config | States (distinct) | Depth | Exit | Result |
|---|---|---|---|---|
| main | 346 | 10 | 0 | no error |
| onestep_bug | 318 | — | 12 | `RepairOnlyOnProvenMismatch` violated ✓ |
| liveness | 346 | 10 | 0 | no error (`EventualConsistencyOrFenced` holds) |

The regression control's counterexample is `RecoveryScan(boot old) → AppendFirst →
Crash → RecoveryScan(boot new, unproven repair)` — the operator-misconfig path needs no
torn writes or exotic cuts to reach.

### Files

- `key_rotation.tla` — the model.
- `key_rotation*.cfg` — see the table above.

---

## `depth_snapshot_bootstrap.tla` — Binance depth snapshot bootstrap (audit MD-BOOK-002)

The second model closing this file's former "Still not modeled" list. `DepthManager`
(`native/include/hengyuan/depth_manager.hpp`) is a single-owner, synchronous,
in-memory state machine: Buffering → (REST snapshot) → Syncing → (replay) → Tracking,
with `start_buffering()` resync back to Buffering. This is NOT a crash-safety model —
there is no persistence and no concurrency here, and none is invented: the
`SnapshotRefreshGate` worker/mailbox (`snapshot_refresh_gate.hpp`) is a separate
subsystem whose own header defers TLA+ modeling of it to a future round, so the whole
fetch+apply cycle is abstracted to one atomic action.

The model is built on the real `U`/`u` window semantics of Binance depthUpdate events:
each event carries `[U, u]` (first/last update id covered), and the replay's continuity
rules (`apply_snapshot()`, lines 147-202) are modeled branch-for-branch: per-event
stale drop (`u <= lastUpdateId`), first-kept bridge check (`U <= lastUpdateId+1`), the
Tracking gap check (`U > last_applied_u + 1` ⇒ re-buffer the gap event itself), and
`OrderBook::apply_delta()`'s outcome never changing the control flow
(`record_apply_result()`, lines 210-221 — the sequence is consumed even on
OutsideWindow/InvalidInput).

### The configs, and what each one MUST report

| Config | Expected result |
|---|---|
| `depth_snapshot_bootstrap.cfg` | **No error found** — the current design |
| `depth_snapshot_bootstrap_drop_before_snapshot_bug.cfg` | **`TrackingImpliesBookConsistent` violated** — regression control #1 |
| `depth_snapshot_bootstrap_overflow_still_track_bug.cfg` | **`NoOverflowThenTracking` violated** — regression control #2 |
| `depth_snapshot_bootstrap_first_bridge_disabled_bug.cfg` | **`FirstBridgeConstraint` violated** — regression control #3 |
| `depth_snapshot_bootstrap_liveness.cfg` | **No error found** — `EventualBootstrap` holds |

```bash
cd formal
java -cp tools/tla2tools.jar tlc2.TLC -config depth_snapshot_bootstrap.cfg depth_snapshot_bootstrap.tla
```

### Why three configs must fail

- **`_drop_before_snapshot_bug`** (`BufferEventsInBuffering = FALSE`) reinstates the
  drop-before-snapshot behavior: events arriving while Buffering are discarded instead
  of buffered for replay. The real replay has NO mid-buffer continuity check after the
  first kept event (lines 172-197), so the hole is invisible to the machine itself:
  it enters Tracking on a book that diverges from a never-drop reference. The violated
  invariant is the bootstrap-complete consistency check (`TrackingImpliesBookConsistent`
  ≈ `book_consistent`); the protection mechanism disabled is the Buffering branch of
  `on_depth_event()` (lines 104-122, tests `BuffersEventsBeforeSnapshot` /
  `OverflowedBufferForcesResyncInsteadOfTracking`).
- **`_overflow_still_track_bug`** (`RejectSnapshotOnOverflow = FALSE`) reinstates
  applying a snapshot over a known-incomplete buffered prefix: the fail-closed overflow
  gate at lines 150-158 is disabled, so the machine enters Tracking with
  `buf_overflowed_` still set — a silent hole it will never notice. Violates
  `NoOverflowThenTracking` (test `OverflowedBufferForcesResyncInsteadOfTracking`).
- **`_first_bridge_disabled_bug`** (`CheckFirstBridge = FALSE`) reinstates entering
  Tracking without verifying `U <= lastUpdateId+1` on the first kept event — a book
  built across the snapshot/stream boundary gap. Violates `FirstBridgeConstraint`
  (lines 181-189, test `ResyncOnGapInBufferedEvents`).

If any of the three ever stops failing, the model has stopped discriminating that
protection, and every clean run of the main config is worth correspondingly less.

**Invariant-to-code map.**

| Invariant | Real code it pins |
|---|---|
| `NoOverflowThenTracking` | `apply_snapshot()`'s overflow rejection, `depth_manager.hpp:150-158` |
| `StaleIncrementsDropped` | the replay's per-event drop, lines 176-179 |
| `FirstBridgeConstraint` | the first-kept bridge check, lines 181-189 |
| `TrackingGapForcesRebuffer` | the Tracking gap branch incl. the re-buffered event itself, lines 124-133 |
| `SequenceAlwaysConsumedOnRejection` | `record_apply_result()` + the unconditional `last_applied_u_ = u`, lines 135-138, 210-221 |
| `TrackingImpliesBookConsistent` | the never-drop reference comparison (`book_consistent`, modeled as `~episode_dropped`) |
| `EventualBootstrap` (liveness) | the resync/fetch loop eventually lands in Tracking |

**The liveness fairness assumption, and why it is honest.** `EventualBootstrap` is
checked with `WF_vars(ApplySnapshot(S))` per snapshot value and a bounded delivery
budget (`MaxDeliveries`, an action guard — the same no-`CONSTRAINT` discipline as every
liveness config in this directory). The fairness corresponds to a real operating fact:
the hot thread polls `needs_snapshot()` every iteration and `SnapshotRefreshGate`
fetches asynchronously, retrying after each failure's finite `kCooldown`
(`snapshot_refresh_gate.hpp`) — so the fetch+apply cycle is eventually taken and
eventually succeeds, and every fetch-reject / bridge-resync cycle consumes the delivery
budget, so after finitely many cycles a fetch lands in Tracking.

### Recorded results (2026-08-09, TLC 2.19, Java 25)

| Config | States (distinct) | Depth | Exit | Result |
|---|---|---|---|---|
| main | 198,202 | 11 | 0 | no error |
| drop_before_snapshot_bug | 48 | — | 12 | `TrackingImpliesBookConsistent` violated ✓ |
| overflow_still_track_bug | 1,208 | — | 12 | `NoOverflowThenTracking` violated ✓ |
| first_bridge_disabled_bug | 1,160 | — | 12 | `FirstBridgeConstraint` violated ✓ |
| liveness | 198,202 | 11 | 0 | no error (`EventualBootstrap` holds) |

The drop-before-snapshot counterexample is four states deep (Init → two dropped
deliveries → snapshot applied over the incomplete prefix), matching how directly the
defect reaches a diverged book.

### Files

- `depth_snapshot_bootstrap.tla` — the model.
- `depth_snapshot_bootstrap*.cfg` — see the table above.

---

## `RoundDActual.tla` — CandidateLease / IntentStore's real public API (Round D)

Models the REAL, shipped Round D API (`native/include/hengyuan/compaction_lease.hpp`,
`compaction_intent_store.hpp`), not a generic file-lease design. Deliberately narrow:
`CreateGenesis` is the only filesystem write in the state space, and there is no
generalized "artifact name" anywhere -- matching the real API, whose one write method
takes no filename parameter at all (it always targets a compile-time-fixed literal).

**Why this one has three regression controls, not one.** Six external Architect
reviews rejected five successive designs before the shipped one (see
`compaction_lease.hpp`'s own top-of-file comment for the full history). Three of those
findings are safety properties with a real, reachable counterexample in an earlier
design, not style opinions -- exactly what `inflight_lifecycle.tla`'s section above
argues is worth a control: a MODEL SWITCH that reintroduces the rejected design and
MUST make the corresponding invariant fail.

### The configs, and what each one MUST report

| Config | Expected result |
|---|---|
| `RoundDActual.cfg` | **No error found** — the current (shipped) design |
| `RoundDActual_v3bug.cfg` | **`NeverWritesIntoWrongObject` violated** — regression control #1 |
| `RoundDActual_v4bug.cfg` | **`NoCapabilityUseAfterInvalidation` violated** — regression control #2 |
| `RoundDActual_v5bug.cfg` | **`NonOwnerReleaseNeverClearsHeld` violated** — regression control #3 |
| `RoundDActual_liveness.cfg` | **No error found** — `EventualResolution` holds |

```bash
cd formal
java -cp tools/tla2tools.jar tlc2.TLC -config RoundDActual.cfg RoundDActual.tla
```

### Why three configs must fail

- **`_v3bug`** (`HandleRelativeIo = FALSE`) reintroduces the design rejected in review
  v3: check identity by path, then separately open the artifact by path again. An
  external rename/recreate between the two steps redirects the write into a directory
  the caller no longer controls -- this is the TOCTOU finding that drove the entire
  v4+ handle-relative redesign. Counterexample: `Acquire → ExternalIdentityChange →
  CreateGenesis` (4 states).
- **`_v4bug`** (`HandleEncapsulated = FALSE`) reintroduces review v4's finding: a
  public accessor exposing the underlying handle let a caller hold a reference past
  the lease's own lifetime. Counterexample: `Acquire → BorrowHandle → ReleaseByOwner →
  UseHandleViaCapability` (5 states) -- the capability gets used after `held` already
  went false.
- **`_v5bug`** (`OwnerThreadCheckedOnRelease = FALSE`) reintroduces review v5's
  finding: `release()` with no owner-thread check, so any thread could clear `held`
  while the owner is still using it. Counterexample: `Acquire → ReleaseByNonOwner` (3
  states) -- the shortest of the three, matching how directly reachable the bug was.

### Recorded results (2026-08-09, TLC 2.19, Java 25)

| Config | States (distinct) | Depth | Result |
|---|---|---|---|
| main | 22 | 7 | no error |
| v3bug | 5 | 4 | `NeverWritesIntoWrongObject` violated ✓ |
| v4bug | 25 | 5 | `NoCapabilityUseAfterInvalidation` violated ✓ |
| v5bug | 5 | 3 | `NonOwnerReleaseNeverClearsHeld` violated ✓ |
| liveness | 22 | 7 | no error (`EventualResolution` holds) |

The entire state space is finite by construction (8 booleans + `createAttempts`
bounded by `MaxCreateAttempts`) -- no `CONSTRAINT` needed anywhere, unlike this file's
other three models, all of which track an unbounded sequence. No saturation check is
needed for the same reason: there is no bound to second-guess.

### What is deliberately NOT in this model

KeyRing pin/retire (a separate subsystem with its own real coverage --
`test_key_ring.cpp`'s `KeyRingDestroyedWithLivePinTerminates` death test already
exercises that fail-closed property directly, against a real `std::terminate()`, which
a TLA+ model would only restate); the `.x1`/`.xgc` phase-raise/GC state machine (Round
D never writes either -- see `RoundEFDesign.tla` below, a genuinely separate model with
no production traceability of its own); and cross-process mutual exclusion (an
OS-level guarantee already exercised for real by
`test_compaction_lease.cpp`'s `CrossProcessMutualExclusion` test against an actual
second process, which a single-process TLA+ model cannot add confidence to).

### Files

- `RoundDActual.tla` — the model.
- `RoundDActual*.cfg` — see the table above.

---

## `RoundEFDesign.tla` — future `.x1`/`.xgc` state machine (independent, no production traceability)

A genuinely separate model, not an extension of `RoundDActual.tla`. It formalizes
rules that already exist as real, unit-tested pure functions in
`native/include/hengyuan/compaction_intent_codec.hpp` (`walk_x1_chain_raw`,
`is_legal_candidate_id_transition`, `is_legal_cleanup_auth_flags`) but for which **no
production write path exists anywhere in this codebase yet** -- Round D deliberately
never calls any of them (see that file's own SCOPE comment).

This inverts the usual grounding direction: everywhere else in this directory, prose
spec grounding precedes the model (see the Scope note at the top of this file). Here,
tested CODE precedes the model, but there is no production BEHAVIOR to check the model
against yet -- so, unlike every other model in this directory, **it has no regression
control**. There is no historical incident to regress against. Do not add a "must
fail" config for it without a real one to point at.

### The config, and what it MUST report

| Config | Expected result |
|---|---|
| `RoundEFDesign.cfg` | **No error found** |
| `RoundEFDesign_liveness.cfg` | **No error found** — `EventuallyTerminal` holds |

```bash
cd formal
java -cp tools/tla2tools.jar tlc2.TLC -config RoundEFDesign.cfg RoundEFDesign.tla
```

### Recorded results (2026-08-09, TLC 2.19, Java 25)

| Config | States (distinct) | Depth | Result |
|---|---|---|---|
| main | 7 | 5 | no error |
| liveness | 7 | 5 | no error (`EventuallyTerminal` holds) |

A tiny, fully-enumerable state space (5 phases × 4 seq values × 2 idsBound × 2
gcAuthorized, most of it unreachable) -- no `CONSTRAINT` needed here either.

### Files

- `RoundEFDesign.tla` — the model.
- `RoundEFDesign*.cfg` — see the table above.
