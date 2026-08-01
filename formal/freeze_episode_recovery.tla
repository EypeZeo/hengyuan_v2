---------------------------- MODULE freeze_episode_recovery ----------------------------
(* Track C (轨道 C) — L4 §10 freeze-episode vertical slice.
 *
 * Scope: this models the freeze-episode state machine described in
 * docs/BINANCE_PRIVATE_REST_L4_SPEC.md §10's freeze payload block
 * (RateLimitFreezePayload / FreezeProbeAttemptPayload / FreezeClearPayload /
 * FreezeWaitArmPayload / FreezeWaitSatisfiedPayload) and the corresponding
 * three already-documented behavioral invariants in
 * docs/SPEC_INVARIANTS.md's "崩溃恢复 / Freeze 子系统" section:
 * FreezeProbeCredit (8-attempt cap), wait_ok (round 30: sink must
 * independently re-verify elapsed wait time, never trust a caller-supplied
 * boolean), and wait_generation (round 31/32: a stale generation's
 * WaitSatisfied must never satisfy a newer generation's clear).
 *
 * durable_log_recovery.tla's own header explicitly disclaims this scope:
 * "Deliberately OUT of scope: ... the freeze/rate-limit subsystem ... those
 * are the round 30-72 subject matter this work only has changelog-level (not
 * full prose+pseudocode-level) grounding on." This is a NEW, separate model,
 * not an extension of that one — grounding for JUST the freeze-episode slice
 * now exists (docs/BINANCE_PRIVATE_REST_L4_SPEC.md:2570-2816), but the much
 * larger compaction/generation-switch/seal-journal machinery in the rest of
 * §10 still does not have full prose+pseudocode-level grounding here and is
 * NOT modeled by this file either.
 *
 * ---------------------------------------------------------------------------
 * MODELING CONVENTIONS (mirrors durable_log_recovery.tla's own conventions)
 * ---------------------------------------------------------------------------
 *
 * Durable (survive Crash): freezeEpoch, waitGeneration, frozen, probeAttempts,
 *     armed, satisfied, clearKind, permanentLatch.
 * Volatile (wiped by Crash/OperatorRepairRestart): armedThisSession,
 *     waitElapsedEnough, probeAckedThisAttempt, fenced.
 * Ghost (survives Crash, no process reads it directly except through the
 *     specific channels below): probesSent is a ghost counter in the same
 *     spirit as the base model's sendCount — nothing in this model reads it
 *     back to make a decision, it exists purely so an invariant CAN observe
 *     "how many real /time sends happened" against structural evidence.
 *
 * waitElapsedEnough is THE round-30 hinge: it is real, ground-truth elapsed
 * time since the current session's Arm was genuinely Acked. A caller-supplied
 * claim can never substitute for it — the SinkVerifiesWaitIndependently
 * toggle controls whether AckAppend (for a WaitSatisfied frame) actually
 * checks this ghost truth or blindly trusts whatever the frame claims.
 *
 * pendingAppend generalizes durable_log_recovery.tla's own late-fsync
 * StartAppend/Land*/Ack/Fail skeleton across four frame kinds (ProbeAttempt,
 * Arm, WaitSatisfied, Clear) via a `kind` field, exactly as that file's own
 * header names as the natural extension point. Unlike that model, THIS one
 * does not keep a growing append-only durableLog sequence — the freeze
 * subsystem's real recovery (recover_control_plane()) reconstructs only the
 * LATEST record of each kind, not a full replay history, so scalar durable
 * state (armed/satisfied/clearKind) is the more faithful choice here, not
 * just a simplification for state-space size. One consequence: an abandoned
 * write's durable effect (the DurableEffect operator below) is applied
 * directly at LAND time rather than deferred to a separate Recover() action
 * that scans a log — there is no log to scan. The safety-relevant nuance this
 * model exists to check (a forged WaitSatisfied becoming durably visible, a
 * stale generation being accepted) is unaffected by exactly when the state
 * becomes visible, only by whether the guard let it happen at all.
 *
 * A cleared episode's armed/satisfied/probeAttempts durable state is
 * deliberately NOT wiped by the Clear action itself (only clearKind and
 * frozen change) — it stays queryable so NoPrematureConservativeClear and
 * WaitSatisfiedOnlyForCurrentGeneration can check what justified the clear
 * AFTER the clear has happened. It IS wiped by the next DetectFreeze
 * (starting a fresh episode), at the same moment clearKind resets to "None",
 * so the two invariants below are never checking stale cross-episode state.
 *
 * KNOWN RESOLUTION LIMIT: ClearViaProbeVerified's actual ground-truth
 * condition (serverTime genuinely >= deadline) is NOT modeled — the guard
 * only requires probeAttempts > 0 (some attempt genuinely made). Neither of
 * this model's two regression targets (round 30, round 31/32) involves the
 * ProbeVerified path, so giving it a full ghost-truth treatment the way
 * WaitSatisfied gets one would add model complexity without adding
 * discriminating power for a real historical bug. Similarly, this model does
 * not reproduce the round-16/17 L4-depends-on-L5 circular-dependency bug
 * (which sink method gets called is outside what this model's variables can
 * express) — FailAppend here unconditionally fences, matching the current
 * (post round-17) design only.
 *
 * ---------------------------------------------------------------------------
 * CONSTANT TOGGLES (both TRUE-by-design; FALSE reintroduces a historical bug)
 * ---------------------------------------------------------------------------
 * SinkVerifiesWaitIndependently   = FALSE -> round-30: a WaitSatisfied frame
 *     can be Acked (become durable) without the sink checking real elapsed
 *     time at all — the caller's claim is trusted outright.
 * PromoteLegacyGenerationCorrectly = FALSE -> round-31/32: a
 *     ConservativeWaitCompleted clear accepts a WaitSatisfied bound to any
 *     generation <= the current one, not just an exact match — a stale
 *     generation's evidence can satisfy a newer generation's requirement.
 * Both must produce a counterexample; if either stops doing so, the model has
 * stopped discriminating and must not be trusted until fixed.
 *)
EXTENDS Naturals, TLC

CONSTANTS
    SinkVerifiesWaitIndependently,
    PromoteLegacyGenerationCorrectly,
    MaxCrashes,          \* modeling budget, NOT a design bound
    MaxAppendFailures,   \* modeling budget, NOT a design bound
    MaxProbeTries,       \* modeling budget for StartProbeAttempt -- deliberately
                         \* allowed to exceed 8 so ProbeAttemptsNeverExceedCap is
                         \* actually falsifiable-in-principle, not vacuous
    MaxEpisodeEvents     \* shared modeling budget for DetectFreeze + BumpGeneration

ASSUME SinkVerifiesWaitIndependently \in BOOLEAN
ASSUME PromoteLegacyGenerationCorrectly \in BOOLEAN
ASSUME MaxCrashes \in Nat
ASSUME MaxAppendFailures \in Nat
ASSUME MaxProbeTries \in Nat
ASSUME MaxEpisodeEvents \in Nat

FrameKinds == {"ProbeAttempt", "Arm", "WaitSatisfied", "Clear"}
ClearKinds == {"None", "ProbeVerified", "ConservativeWaitCompleted", "OperatorAuthorized"}

VARIABLES
    freezeEpoch,           \* durable
    waitGeneration,        \* durable
    frozen,                \* durable -- an active (uncleared) episode exists
    probeAttempts,         \* durable -- Acked-probe count for the current episode
    armed,                 \* durable -- [active, gen] of the last Acked Arm
    satisfied,             \* durable -- [active, gen, groundTruthOk] of the last
                           \* Acked WaitSatisfied
    clearKind,             \* durable -- ClearKinds for the current/most-recent episode
    permanentLatch,        \* durable -- source=2 permanent freeze
    armedThisSession,      \* volatile -- process-local arm_ack_steady presence
    waitElapsedEnough,     \* ghost -- real elapsed-time truth since Arm's Ack
                           \* THIS session (the round-30 hinge)
    probeAckedThisAttempt, \* volatile -- gates the /time send (round-26 P0)
    probesSent,            \* ghost counter -- real /time sends, for a structural check
    pendingAppend,         \* the single outstanding durable write (late-fsync skeleton)
    fenced,                \* volatile process-lifetime writer fence
    crashesLeft,           \* modeling device: finite-by-GUARD, not by CONSTRAINT
    failsLeft,
    probeTriesLeft,
    episodeEventsLeft

vars == <<freezeEpoch, waitGeneration, frozen, probeAttempts, armed, satisfied,
          clearKind, permanentLatch, armedThisSession, waitElapsedEnough,
          probeAckedThisAttempt, probesSent, pendingAppend, fenced,
          crashesLeft, failsLeft, probeTriesLeft, episodeEventsLeft>>

PendingRecs ==
    [kind        : FrameKinds,
     gen         : Nat,           \* used by Arm/WaitSatisfied
     groundTruthOk : BOOLEAN,     \* used by WaitSatisfied -- captures
                                  \* waitElapsedEnough at append time
     ck          : ClearKinds,    \* used by Clear
     landed      : BOOLEAN,
     obs         : {"waiting", "abandoned"},
     active      : BOOLEAN]

\* Canonical idle-writer value, same rationale as durable_log_recovery.tla's
\* IdleWriter: one representation for "nothing pending" so semantically
\* identical states fingerprint identically. Non-active fields are arbitrary.
IdlePending == [kind |-> "ProbeAttempt", gen |-> 0, groundTruthOk |-> FALSE,
                ck |-> "None", landed |-> FALSE, obs |-> "waiting", active |-> FALSE]

NoArm       == [active |-> FALSE, gen |-> 0]
NoSatisfied == [active |-> FALSE, gen |-> 0, groundTruthOk |-> FALSE]

WriterIdle == ~pendingAppend.active   \* single owner actor (spec §9)
CanWrite   == WriterIdle /\ ~fenced

TypeOK ==
    /\ freezeEpoch \in Nat
    /\ waitGeneration \in Nat
    /\ frozen \in BOOLEAN
    /\ probeAttempts \in Nat
    /\ armed \in [active: BOOLEAN, gen: Nat]
    /\ satisfied \in [active: BOOLEAN, gen: Nat, groundTruthOk: BOOLEAN]
    /\ clearKind \in ClearKinds
    /\ permanentLatch \in BOOLEAN
    /\ armedThisSession \in BOOLEAN
    /\ waitElapsedEnough \in BOOLEAN
    /\ probeAckedThisAttempt \in BOOLEAN
    /\ probesSent \in Nat
    /\ pendingAppend \in PendingRecs
    /\ fenced \in BOOLEAN
    /\ crashesLeft \in 0..MaxCrashes
    /\ failsLeft \in 0..MaxAppendFailures
    /\ probeTriesLeft \in 0..MaxProbeTries
    /\ episodeEventsLeft \in 0..MaxEpisodeEvents

Init ==
    /\ freezeEpoch = 0
    /\ waitGeneration = 0
    /\ frozen = FALSE
    /\ probeAttempts = 0
    /\ armed = NoArm
    /\ satisfied = NoSatisfied
    /\ clearKind = "None"
    /\ permanentLatch = FALSE
    /\ armedThisSession = FALSE
    /\ waitElapsedEnough = FALSE
    /\ probeAckedThisAttempt = FALSE
    /\ probesSent = 0
    /\ pendingAppend = IdlePending
    /\ fenced = FALSE
    /\ crashesLeft = MaxCrashes
    /\ failsLeft = MaxAppendFailures
    /\ probeTriesLeft = MaxProbeTries
    /\ episodeEventsLeft = MaxEpisodeEvents

(***************************************************************************)
(* Episode lifecycle: a fresh freeze starting, or a new unknown-429 arriving*)
(* mid-episode (the round-31/32 scenario -- generation bump while still     *)
(* open, making existing Arm/WaitSatisfied evidence stale).                 *)
(***************************************************************************)

DetectFreeze ==
    /\ ~frozen
    /\ episodeEventsLeft > 0
    /\ episodeEventsLeft' = episodeEventsLeft - 1
    /\ frozen' = TRUE
    /\ freezeEpoch' = freezeEpoch + 1
    /\ waitGeneration' = 0
    /\ probeAttempts' = 0
    /\ armed' = NoArm
    /\ satisfied' = NoSatisfied
    /\ clearKind' = "None"
    /\ permanentLatch' \in BOOLEAN   \* nondeterministic: source=2 or timed
    /\ armedThisSession' = FALSE
    /\ waitElapsedEnough' = FALSE
    /\ probeAckedThisAttempt' = FALSE
    /\ UNCHANGED <<probesSent, pendingAppend, fenced, crashesLeft, failsLeft,
                   probeTriesLeft>>

BumpGeneration ==
    /\ frozen
    /\ clearKind = "None"
    /\ episodeEventsLeft > 0
    /\ episodeEventsLeft' = episodeEventsLeft - 1
    /\ waitGeneration' = waitGeneration + 1
    /\ UNCHANGED <<freezeEpoch, frozen, probeAttempts, armed, satisfied,
                   clearKind, permanentLatch, armedThisSession, waitElapsedEnough,
                   probeAckedThisAttempt, probesSent, pendingAppend, fenced,
                   crashesLeft, failsLeft, probeTriesLeft>>

(***************************************************************************)
(* Durable writes: issuing them.                                           *)
(***************************************************************************)

\* FreezeProbeCredit's 8-attempt cap; "not for source=2" (spec:2571,2617).
StartProbeAttempt ==
    /\ CanWrite
    /\ frozen
    /\ ~permanentLatch
    /\ probeAttempts < 8
    /\ probeTriesLeft > 0
    /\ probeTriesLeft' = probeTriesLeft - 1
    /\ pendingAppend' = [IdlePending EXCEPT !.kind = "ProbeAttempt", !.active = TRUE]
    /\ UNCHANGED <<freezeEpoch, waitGeneration, frozen, probeAttempts, armed,
                   satisfied, clearKind, permanentLatch, armedThisSession,
                   waitElapsedEnough, probeAckedThisAttempt, probesSent, fenced,
                   crashesLeft, failsLeft, episodeEventsLeft>>

ArmWait ==
    /\ CanWrite
    /\ frozen
    /\ clearKind = "None"
    /\ ~permanentLatch
    /\ pendingAppend' = [IdlePending EXCEPT !.kind = "Arm", !.gen = waitGeneration,
                                             !.active = TRUE]
    /\ UNCHANGED <<freezeEpoch, waitGeneration, frozen, probeAttempts, armed,
                   satisfied, clearKind, permanentLatch, armedThisSession,
                   waitElapsedEnough, probeAckedThisAttempt, probesSent, fenced,
                   crashesLeft, failsLeft, probeTriesLeft, episodeEventsLeft>>

\* THE ROUND-30 HINGE, part 1: claiming a wait is complete. groundTruthOk is
\* captured from the REAL ghost clock at this moment, not from any caller
\* input -- what happens with that captured value is decided at Ack time.
ClaimWaitSatisfied ==
    /\ CanWrite
    /\ frozen
    /\ clearKind = "None"
    /\ armedThisSession
    /\ pendingAppend' = [IdlePending EXCEPT !.kind = "WaitSatisfied",
                                             !.gen = armed.gen,
                                             !.groundTruthOk = waitElapsedEnough,
                                             !.active = TRUE]
    /\ UNCHANGED <<freezeEpoch, waitGeneration, frozen, probeAttempts, armed,
                   satisfied, clearKind, permanentLatch, armedThisSession,
                   waitElapsedEnough, probeAckedThisAttempt, probesSent, fenced,
                   crashesLeft, failsLeft, probeTriesLeft, episodeEventsLeft>>

\* Ground truth for ClearViaProbeVerified is out of this model's resolution
\* (see header) -- only requires a genuine attempt to have been made.
ClearViaProbeVerified ==
    /\ CanWrite
    /\ frozen
    /\ clearKind = "None"
    /\ ~permanentLatch
    /\ probeAttempts > 0
    /\ pendingAppend' = [IdlePending EXCEPT !.kind = "Clear",
                                             !.ck = "ProbeVerified", !.active = TRUE]
    /\ UNCHANGED <<freezeEpoch, waitGeneration, frozen, probeAttempts, armed,
                   satisfied, clearKind, permanentLatch, armedThisSession,
                   waitElapsedEnough, probeAckedThisAttempt, probesSent, fenced,
                   crashesLeft, failsLeft, probeTriesLeft, episodeEventsLeft>>

\* Starting side: the caller believes it has SOME WaitSatisfied grounds to
\* clear. THE ROUND-31/32 HINGE (whether that grounds' generation is still
\* current) is deliberately NOT checked here -- it is re-verified at Ack time
\* below, because BumpGeneration is not serialized against an in-flight
\* append (a fresh 429 can arrive at any time, matching real network
\* behavior), so a check here could pass and then go stale before landing.
\* The sink independently re-verifying at the moment of durability, not at
\* the moment of request, is the same discipline round-30's wait_ok fix uses.
ClearViaConservativeWaitCompleted ==
    /\ CanWrite
    /\ frozen
    /\ clearKind = "None"
    /\ ~permanentLatch
    /\ satisfied.active
    /\ pendingAppend' = [IdlePending EXCEPT !.kind = "Clear",
                                             !.ck = "ConservativeWaitCompleted",
                                             !.active = TRUE]
    /\ UNCHANGED <<freezeEpoch, waitGeneration, frozen, probeAttempts, armed,
                   satisfied, clearKind, permanentLatch, armedThisSession,
                   waitElapsedEnough, probeAckedThisAttempt, probesSent, fenced,
                   crashesLeft, failsLeft, probeTriesLeft, episodeEventsLeft>>

\* "ONLY legal clear for source=2" (spec:2573).
ClearViaOperatorAuthorized ==
    /\ CanWrite
    /\ frozen
    /\ clearKind = "None"
    /\ permanentLatch
    /\ pendingAppend' = [IdlePending EXCEPT !.kind = "Clear",
                                             !.ck = "OperatorAuthorized", !.active = TRUE]
    /\ UNCHANGED <<freezeEpoch, waitGeneration, frozen, probeAttempts, armed,
                   satisfied, clearKind, permanentLatch, armedThisSession,
                   waitElapsedEnough, probeAckedThisAttempt, probesSent, fenced,
                   crashesLeft, failsLeft, probeTriesLeft, episodeEventsLeft>>

(***************************************************************************)
(* The /time send: illegal until the ProbeAttempt frame is Acked (round-26  *)
(* P0 -- Ack is the linearization point that consumes probe budget).        *)
(***************************************************************************)

SendTimeProbe ==
    /\ probeAckedThisAttempt
    /\ probeAckedThisAttempt' = FALSE
    /\ probesSent' = probesSent + 1
    /\ UNCHANGED <<freezeEpoch, waitGeneration, frozen, probeAttempts, armed,
                   satisfied, clearKind, permanentLatch, armedThisSession,
                   waitElapsedEnough, pendingAppend, fenced, crashesLeft,
                   failsLeft, probeTriesLeft, episodeEventsLeft>>

(***************************************************************************)
(* Applying a landed frame's durable effect. Shared by AckAppend (the       *)
(* caller-observed path) and LandAfterAbandon (the late-fsync path) -- see  *)
(* the header note on why this model applies effects at land time rather    *)
(* than via a separate log-scanning Recover().                             *)
(*                                                                          *)
(* Clear deliberately does NOT reset probeAttempts/armed/satisfied -- they  *)
(* stay queryable so NoPrematureConservativeClear /                        *)
(* WaitSatisfiedOnlyForCurrentGeneration can check what justified the       *)
(* clear. DetectFreeze (starting the next episode) is what resets them.     *)
(***************************************************************************)

\* THE ROUND-31/32 HINGE. Under the correct toggle, a WaitSatisfied's
\* generation must equal the CURRENT one exactly; under the bug, any
\* generation at or before the current one is (wrongly) accepted -- the
\* legacy gen=0->1 upgrade path re-matching a stale generation's evidence.
GenMatches(g, cur) ==
    IF PromoteLegacyGenerationCorrectly THEN g = cur ELSE g <= cur

\* Re-verified at the moment of durability (Ack/late-land), not at request
\* time -- see ClearViaConservativeWaitCompleted's own comment for why.
ClearIsJustified(p) ==
    (p.kind = "Clear" /\ p.ck = "ConservativeWaitCompleted") =>
        (satisfied.active /\ GenMatches(satisfied.gen, waitGeneration))

DurableEffect(p) ==
    IF p.kind = "ProbeAttempt" THEN
        [probeAttempts |-> probeAttempts + 1, armed |-> armed,
         satisfied |-> satisfied, clearKind |-> clearKind, frozen |-> frozen]
    ELSE IF p.kind = "Arm" THEN
        [probeAttempts |-> probeAttempts, armed |-> [active |-> TRUE, gen |-> p.gen],
         satisfied |-> satisfied, clearKind |-> clearKind, frozen |-> frozen]
    ELSE IF p.kind = "WaitSatisfied" THEN
        [probeAttempts |-> probeAttempts, armed |-> armed,
         satisfied |-> [active |-> TRUE, gen |-> p.gen, groundTruthOk |-> p.groundTruthOk],
         clearKind |-> clearKind, frozen |-> frozen]
    ELSE \* "Clear"
        [probeAttempts |-> probeAttempts, armed |-> armed, satisfied |-> satisfied,
         clearKind |-> p.ck, frozen |-> FALSE]

\* append_durable() returns Acked -- REQUIRES landed (storage honest about
\* success), same asymmetry as durable_log_recovery.tla's own AckAppend.
\*
\* THE ROUND-30 GUARD lives here: when SinkVerifiesWaitIndependently is TRUE,
\* this action is simply NOT ENABLED for a WaitSatisfied frame whose captured
\* groundTruthOk is FALSE -- a forged claim can never become durable. When
\* FALSE, the implication is vacuously satisfied regardless of groundTruthOk,
\* reproducing the forged-Ack bug.
AckAppend ==
    /\ pendingAppend.active
    /\ pendingAppend.landed
    /\ pendingAppend.obs = "waiting"
    /\ (pendingAppend.kind = "WaitSatisfied" =>
            (SinkVerifiesWaitIndependently => pendingAppend.groundTruthOk))
    /\ ClearIsJustified(pendingAppend)
    /\ LET eff == DurableEffect(pendingAppend) IN
       /\ probeAttempts' = eff.probeAttempts
       /\ armed' = eff.armed
       /\ satisfied' = eff.satisfied
       /\ clearKind' = eff.clearKind
       /\ frozen' = eff.frozen
    /\ pendingAppend' = IdlePending
    /\ probeAckedThisAttempt' = IF pendingAppend.kind = "ProbeAttempt" THEN TRUE
                                 ELSE probeAckedThisAttempt
    /\ armedThisSession' = IF pendingAppend.kind = "Arm" THEN TRUE ELSE armedThisSession
    /\ waitElapsedEnough' = IF pendingAppend.kind = "Arm" THEN FALSE ELSE waitElapsedEnough
    /\ UNCHANGED <<freezeEpoch, waitGeneration, permanentLatch, probesSent, fenced,
                   crashesLeft, failsLeft, probeTriesLeft, episodeEventsLeft>>

(***************************************************************************)
(* The physical write completing -- or not. Mirrors                        *)
(* durable_log_recovery.tla's three-way split exactly.                     *)
(***************************************************************************)

LandForWaitingCaller ==
    /\ pendingAppend.active
    /\ ~pendingAppend.landed
    /\ pendingAppend.obs = "waiting"
    /\ pendingAppend' = [pendingAppend EXCEPT !.landed = TRUE]
    /\ UNCHANGED <<freezeEpoch, waitGeneration, frozen, probeAttempts, armed,
                   satisfied, clearKind, permanentLatch, armedThisSession,
                   waitElapsedEnough, probeAckedThisAttempt, probesSent, fenced,
                   crashesLeft, failsLeft, probeTriesLeft, episodeEventsLeft>>

\* THE LATE FSYNC. Bytes complete after the caller was told Failed, or after
\* the caller's process died. The durable effect applies here directly (see
\* the DurableEffect header note) -- but the SESSION-local volatile fields
\* (armedThisSession/waitElapsedEnough/probeAckedThisAttempt) do NOT update:
\* an abandoned write's caller is, by definition, not the live session
\* anymore. This is exactly spec:2745's own rule: "Abandoned across process
\* restart if no matching WaitSatisfied was Acked (live sink has no
\* arm_ack_steady for a prior Arm)".
LandAfterAbandon ==
    /\ pendingAppend.active
    /\ ~pendingAppend.landed
    /\ pendingAppend.obs = "abandoned"
    /\ (pendingAppend.kind = "WaitSatisfied" =>
            (SinkVerifiesWaitIndependently => pendingAppend.groundTruthOk))
    /\ ClearIsJustified(pendingAppend)
    /\ LET eff == DurableEffect(pendingAppend) IN
       /\ probeAttempts' = eff.probeAttempts
       /\ armed' = eff.armed
       /\ satisfied' = eff.satisfied
       /\ clearKind' = eff.clearKind
       /\ frozen' = eff.frozen
    /\ pendingAppend' = IdlePending
    /\ UNCHANGED <<freezeEpoch, waitGeneration, permanentLatch, armedThisSession,
                   waitElapsedEnough, probeAckedThisAttempt, probesSent, fenced,
                   crashesLeft, failsLeft, probeTriesLeft, episodeEventsLeft>>

\* The write genuinely never lands (power cut, device dropped it).
LoseAfterAbandon ==
    /\ pendingAppend.active
    /\ ~pendingAppend.landed
    /\ pendingAppend.obs = "abandoned"
    /\ pendingAppend' = IdlePending
    /\ UNCHANGED <<freezeEpoch, waitGeneration, frozen, probeAttempts, armed,
                   satisfied, clearKind, permanentLatch, armedThisSession,
                   waitElapsedEnough, probeAckedThisAttempt, probesSent, fenced,
                   crashesLeft, failsLeft, probeTriesLeft, episodeEventsLeft>>

\* append_durable() returns Failed. Enabled regardless of `landed` -- a Failed
\* return never proves the bytes did not land. Unconditionally fences (this
\* model only represents the current, post-round-17 design -- see header).
FailAppend ==
    /\ pendingAppend.active
    /\ pendingAppend.obs = "waiting"
    /\ failsLeft > 0
    /\ failsLeft' = failsLeft - 1
    /\ fenced' = TRUE
    /\ pendingAppend' = IF pendingAppend.landed
                        THEN IdlePending
                        ELSE [pendingAppend EXCEPT !.obs = "abandoned"]
    /\ UNCHANGED <<freezeEpoch, waitGeneration, frozen, probeAttempts, armed,
                   satisfied, clearKind, permanentLatch, armedThisSession,
                   waitElapsedEnough, probeAckedThisAttempt, probesSent,
                   crashesLeft, probeTriesLeft, episodeEventsLeft>>

(***************************************************************************)
(* Crash, restart, and process-lifetime fencing.                           *)
(***************************************************************************)

Crash ==
    /\ crashesLeft > 0
    /\ crashesLeft' = crashesLeft - 1
    /\ fenced' = FALSE
    /\ armedThisSession' = FALSE
    /\ waitElapsedEnough' = FALSE
    /\ probeAckedThisAttempt' = FALSE
    /\ pendingAppend' = IF ~pendingAppend.active \/ pendingAppend.landed
                        THEN IdlePending
                        ELSE [pendingAppend EXCEPT !.obs = "abandoned"]
    /\ UNCHANGED <<freezeEpoch, waitGeneration, frozen, probeAttempts, armed,
                   satisfied, clearKind, permanentLatch, probesSent,
                   failsLeft, probeTriesLeft, episodeEventsLeft>>

\* Same effect as Crash, but a distinct, weakly-fair action (see Fairness) --
\* the honest statement "we assume operators eventually respond."
OperatorRepairRestart ==
    /\ fenced
    /\ fenced' = FALSE
    /\ armedThisSession' = FALSE
    /\ waitElapsedEnough' = FALSE
    /\ probeAckedThisAttempt' = FALSE
    /\ pendingAppend' = IF ~pendingAppend.active \/ pendingAppend.landed
                        THEN IdlePending
                        ELSE [pendingAppend EXCEPT !.obs = "abandoned"]
    /\ UNCHANGED <<freezeEpoch, waitGeneration, frozen, probeAttempts, armed,
                   satisfied, clearKind, permanentLatch, probesSent,
                   crashesLeft, failsLeft, probeTriesLeft, episodeEventsLeft>>

\* Ghost: real steady-clock time elapsing while armed this session.
WaitTick ==
    /\ armedThisSession
    /\ ~waitElapsedEnough
    /\ waitElapsedEnough' = TRUE
    /\ UNCHANGED <<freezeEpoch, waitGeneration, frozen, probeAttempts, armed,
                   satisfied, clearKind, permanentLatch, armedThisSession,
                   probeAckedThisAttempt, probesSent, pendingAppend, fenced,
                   crashesLeft, failsLeft, probeTriesLeft, episodeEventsLeft>>

Next ==
    \/ DetectFreeze
    \/ BumpGeneration
    \/ StartProbeAttempt
    \/ SendTimeProbe
    \/ ArmWait
    \/ WaitTick
    \/ ClaimWaitSatisfied
    \/ ClearViaProbeVerified
    \/ ClearViaConservativeWaitCompleted
    \/ ClearViaOperatorAuthorized
    \/ LandForWaitingCaller
    \/ LandAfterAbandon
    \/ LoseAfterAbandon
    \/ AckAppend
    \/ FailAppend
    \/ Crash
    \/ OperatorRepairRestart

Spec == Init /\ [][Next]_vars

\* No StateConstraint is needed for either the safety or the liveness config
\* (unlike durable_log_recovery.tla, which bounds an unboundedly-growing
\* append-only log): every Nat-valued variable here is already bounded by an
\* explicit action guard tied to a CONSTANT budget (probeAttempts < 8;
\* freezeEpoch/waitGeneration only advance via DetectFreeze/BumpGeneration,
\* both gated by episodeEventsLeft; probesSent is bounded transitively by
\* probeTriesLeft). The state graph is finite by construction, which is what
\* makes the liveness check sound without a CONSTRAINT line.

(***************************************************************************)
(* Safety invariants -- a violation here means the DESIGN is wrong.         *)
(***************************************************************************)

ProbeAttemptsNeverExceedCap == probeAttempts <= 8

\* ROUND-30 REGRESSION TARGET. A ConservativeWaitCompleted clear must be
\* backed by a WaitSatisfied record that was genuinely ground-truth-verified
\* at Ack time, never one the sink merely trusted a caller's claim for.
NoPrematureConservativeClear ==
    clearKind = "ConservativeWaitCompleted" =>
        (satisfied.active /\ satisfied.groundTruthOk)

\* ROUND-31/32 REGRESSION TARGET. The WaitSatisfied record backing a
\* ConservativeWaitCompleted clear must be bound to the CURRENT generation
\* exactly -- a stale generation must never satisfy a newer one.
WaitSatisfiedOnlyForCurrentGeneration ==
    clearKind = "ConservativeWaitCompleted" =>
        (satisfied.active /\ satisfied.gen = waitGeneration)

\* "ONLY legal clear for source=2" (spec:2573) -- structurally guaranteed by
\* the three ClearVia* actions' own guards; kept as a checked invariant so a
\* future edit to those guards cannot silently break this.
PermanentLatchOnlyOperatorClear ==
    permanentLatch => clearKind \in {"None", "OperatorAuthorized"}

(***************************************************************************)
(* Reachability probe -- NOT an invariant of anything. Run in its own cfg;  *)
(* TLC MUST report a violation. armed.active alongside ~armedThisSession    *)
(* and ~satisfied.active is the unique signature of "this session Armed,    *)
(* then crashed before ever claiming WaitSatisfied" -- spec:2745's          *)
(* "abandoned across process restart" rule. If TLC does NOT violate this,   *)
(* that rule is never actually exercised by this model.                    *)
(***************************************************************************)
AbandonedArmUnreachable ==
    ~(armed.active /\ ~armedThisSession /\ ~satisfied.active)

(***************************************************************************)
(* Liveness. Progress is measured against `frozen`, which is durable        *)
(* (survives Crash) -- an active episode always eventually resolves.        *)
(***************************************************************************)

EventualFreezeResolution == frozen => <>(~frozen)

Fairness ==
    /\ WF_vars(StartProbeAttempt)
    /\ WF_vars(SendTimeProbe)
    /\ WF_vars(ArmWait)
    /\ WF_vars(WaitTick)
    /\ WF_vars(ClaimWaitSatisfied)
    /\ WF_vars(ClearViaProbeVerified \/ ClearViaConservativeWaitCompleted
               \/ ClearViaOperatorAuthorized)
    /\ WF_vars(LandForWaitingCaller)
    /\ WF_vars(AckAppend)
    /\ WF_vars(LandAfterAbandon \/ LoseAfterAbandon)
    /\ WF_vars(OperatorRepairRestart)
    \* Deliberately NO fairness on Crash, FailAppend, DetectFreeze, or
    \* BumpGeneration -- matching durable_log_recovery.tla's own discipline:
    \* never assume a crash happens, storage fails, or (here) a fresh
    \* rate-limit signal arrives at all/again. Bounded budgets
    \* (crashesLeft/failsLeft/probeTriesLeft/episodeEventsLeft) are what make
    \* the state graph finite, not fairness and not a CONSTRAINT.

SpecLive == Init /\ [][Next]_vars /\ Fairness

====
