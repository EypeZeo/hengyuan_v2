# formal/ — TLA+ crash-recovery models

Layer 3 of `docs/SPEC_INVARIANTS.md`'s mechanization plan: mechanically verify
crash-safety properties of the durable-log design in
`docs/SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md` / `docs/BINANCE_PRIVATE_REST_L4_SPEC.md`
with TLC (exhaustive model checking) instead of relying on another round of human/LLM
review to notice a contradiction.

Two independent models live here: `durable_log_recovery.tla` (the OrderEvent/Gate 8-9
crash-safety boundary) and `freeze_episode_recovery.tla` (the L4 §10 freeze-episode
probe/Arm/WaitSatisfied/Clear state machine, further down this file). Each has its own
config table and MUST be run separately.

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
