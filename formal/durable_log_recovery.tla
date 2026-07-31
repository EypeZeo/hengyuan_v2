---------------------------- MODULE durable_log_recovery ----------------------------
(* Layer 3 of docs/SPEC_INVARIANTS.md's mechanization plan.
 *
 * Scope: this models the crash-safety core of
 * docs/SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md's Gate 8/9 "OrderSubmitPrepared"
 * boundary (spec revision 14, round 13) — the mechanism that took ~13 review
 * rounds to converge on: durably ACK a conservative "about to send" fact BEFORE
 * the actual network call, so that ANY process death after that ACK is recovered
 * as Ambiguous (never assumed unsent, never assumed sent) — closing the class of
 * bug where a crash near the network boundary could cause a blind re-POST and a
 * duplicate order on the exchange.
 *
 * Deliberately OUT of scope: compaction, GC, seal/journal, the freeze/rate-limit
 * subsystem, and multi-COID capacity limits (kMaxInFlight, EscalatedLedger).
 * Those are the round 30-72 subject matter this work only has changelog-level
 * (not full prose+pseudocode-level) grounding on — modeling them without that
 * depth would produce a spec that looks rigorous but encodes guesses, which is
 * worse than not modeling them.
 *
 * ---------------------------------------------------------------------------
 * ROUND 2 (this revision) — what changed and why
 * ---------------------------------------------------------------------------
 *
 * (1) THE DURABLE APPEND IS NO LONGER ATOMIC. The previous revision performed
 *     "append the frame" and "apply the caller-side state transition" in a
 *     single TLA+ step, which made the single most important crash window in
 *     the entire design inexpressible. The spec's own revision 14 P0 states:
 *
 *         "a `Failed` append may still have completed physically ... A failed
 *          prepared append fences/stops and leaves the COID tracked; next-process
 *          recovery treats any complete prepared/submitted frame as ambiguous."
 *
 *     So an append is now StartAppend -> {Land*, Ack, Fail}, with a deliberate
 *     ASYMMETRY that is the whole point: AckAppend REQUIRES `landed` (storage is
 *     honest about success), while FailAppend does NOT require `~landed`
 *     (storage lies about failure — the late fsync). `pendingAppend` is NOT
 *     wiped by Crash: a crash kills the caller, not bytes already queued.
 *
 * (2) `NetworkDeliver` GAINED THE `OutcomeLatched` GUARD, fixing a real modeling
 *     bug present in the previous revision: `AppendOutcome` could durably record
 *     "Rejected" (reading exchangeHasOrder = FALSE) and `NetworkDeliver`'s guard
 *     (sendCount >= 1 /\ ~exchangeHasOrder) was STILL enabled afterwards — so the
 *     durable log could claim Rejected while the exchange held the order. The
 *     old atomicity hid this because nothing compared the record to ground truth;
 *     `OutcomeMatchesGroundTruth` below now does.
 *
 * (3) `NoBlindResendWhenAlreadyPrepared` WAS REMOVED, REPLACED BY
 *     `SendImpliesDurablePrepared`. This is not a weakening — read this before
 *     concluding otherwise. The old invariant said "a coid in state Intent has no
 *     durable records". Once late fsync is modeled that is LEGITIMATELY false:
 *     StartPrepared -> caller observes Failed (state stays Intent, per §6.2) ->
 *     bytes land afterwards, and now Intent coexists with a Prepared frame. The
 *     design is still safe there, but what makes it safe is the WRITER FENCE, not
 *     that structural property. `SendImpliesDurablePrepared` states the actual
 *     Gate-9 claim ("no send without a durable Prepared frame first") and
 *     survives late fsync.
 *
 * (4) ADDED: the writer fence (§6.1.1.1 — any Failed append permanently fences
 *     the writer for that process lifetime), `OperatorRepairRestart`, three
 *     modeling-sanity invariants, and a liveness property with explicit fairness.
 *
 * ---------------------------------------------------------------------------
 * MODELING CONVENTIONS
 * ---------------------------------------------------------------------------
 *
 * Volatile (wiped by Crash): state, sendAttempted, fenced.
 * Durable (survives Crash):  durableLog.
 * Ghost/physical reality (survives Crash, no process can read it directly):
 *                            sendCount, exchangeHasOrder, and the `landed` flag
 *                            inside pendingAppend.
 *
 * No action modeling PROCESS behavior reads ground truth except through a
 * response: `StartOutcome`/`StartReconcile` read exchangeHasOrder because an
 * honest HTTP response necessarily reports it — that is what "not ambiguous"
 * means. `NetworkDeliver`'s guard constrains REALITY, not the process, so it may
 * read the log. `Send`'s guard reads only volatile process memory, never
 * sendCount — a real process has no oracle telling it "I already sent this",
 * which is precisely the problem Gate 9 exists to work around.
 *
 * KNOWN RESOLUTION LIMIT: because the writer is serialized on WriterIdle, a
 * late-landing frame is always appended before any subsequent frame. The
 * sharper hazard §6.1.1.1 describes — a late write occupying a sequence number
 * a later frame already used — needs the sequence-number/hash-chain layer and is
 * out of this model's resolution. What IS covered is a late land racing
 * recovery_scan(), which is the part that can produce a wrong recovered state.
 *
 * ---------------------------------------------------------------------------
 * CONSTANT TOGGLES (both FALSE-by-design; TRUE reintroduces a historical bug)
 * ---------------------------------------------------------------------------
 * RecoverAmbiguousCorrectly = FALSE  -> pre-revision-14: recovery re-derives
 *     "Submitting" instead of "Ambiguous" from a Prepared-only frame.
 * AbortedPreSendCompensation = TRUE  -> pre-revision-14 `AbortedPreSend`: treats
 *     a Failed append as PROOF the operation did not happen, releasing the
 *     registration. This is the toggle that makes the fence load-bearing.
 * Both must produce a NoDoubleSend counterexample; if either stops doing so, the
 * model has stopped discriminating and must not be trusted until fixed.
 *)
EXTENDS Naturals, Sequences, TLC

CONSTANTS
    RecoverAmbiguousCorrectly,
    AbortedPreSendCompensation,
    MaxCrashes,          \* modeling budget, NOT a design bound
    MaxAppendFailures,   \* modeling budget, NOT a design bound
    MaxLogLen            \* StateConstraint bound; a CONSTANT so the saturation
                         \* check (run at two bounds, confirm the state count
                         \* grows and violations stay at zero) is a cfg change
                         \* rather than a model edit -- a bound that silently
                         \* hides violations is the main way a "passing" model
                         \* check gives false confidence.

ASSUME RecoverAmbiguousCorrectly  \in BOOLEAN
ASSUME AbortedPreSendCompensation \in BOOLEAN
ASSUME MaxCrashes \in Nat
ASSUME MaxAppendFailures \in Nat
ASSUME MaxLogLen \in Nat

\* Two client-order-ids is enough to exercise "one crashes/recovers while the
\* other is mid-flight" interleavings without an unnecessarily large state space.
COIDs == {1, 2}

States == {"Intent", "Submitting", "Ambiguous", "Accepted", "Rejected", "Uninitialized"}
FinalStates == {"Accepted", "Rejected"}
FrameKinds == {"Prepared", "Outcome"}

VARIABLES
    state,            \* [COIDs -> States] -- volatile
    durableLog,       \* Seq of frames -- append-only, survives Crash
    sendCount,        \* [COIDs -> Nat] -- ghost history; no process can read it
    exchangeHasOrder, \* [COIDs -> BOOLEAN] -- ghost ground truth
    sendAttempted,    \* [COIDs -> BOOLEAN] -- volatile: has THIS process instance
                      \* already attempted Send in the current Submitting episode
    pendingAppend,    \* the single outstanding durable write; `active` = FALSE
                      \* means the writer is idle. (Modeled as an always-present
                      \* record with an `active` flag rather than a record-or-
                      \* sentinel union: TLC refuses to fingerprint a state where
                      \* a string sentinel is compared against a record.)
                      \* `landed` = bytes are physically on disk (ghost).
                      \* `obs`    = whether a caller is still waiting on the answer.
                      \* NOT wiped by Crash -- only the caller dies, not the bytes.
    fenced,           \* volatile process-lifetime writer fence (§6.1.1.1)
    crashesLeft,      \* modeling device: makes the state graph finite by GUARD,
    failsLeft         \* not by CONSTRAINT -- required for sound liveness (below)

vars == <<state, durableLog, sendCount, exchangeHasOrder, sendAttempted,
          pendingAppend, fenced, crashesLeft, failsLeft>>

PendingRecs ==
    [coid   : COIDs,
     kind   : FrameKinds,
     value  : States \union {"n/a"},
     next   : States,
     landed : BOOLEAN,
     obs    : {"waiting", "abandoned"},
     active : BOOLEAN]

\* The canonical writer-idle value. Every action that clears the writer assigns
\* exactly THIS record rather than flipping `active` in place, so an idle writer
\* has one representation instead of many stale-field variants -- otherwise
\* semantically identical states would fingerprint differently and bloat the
\* state space. The non-`active` fields here are arbitrary and never read.
IdleWriter == [coid |-> 1, kind |-> "Prepared", value |-> "n/a",
               next |-> "Submitting", landed |-> FALSE,
               obs |-> "waiting", active |-> FALSE]

Frame(p)   == [coid |-> p.coid, type |-> p.kind, value |-> p.value]
WriterIdle == ~pendingAppend.active   \* single owner actor, blocking append (§9)
CanWrite   == WriterIdle /\ ~fenced

TypeOK ==
    /\ state \in [COIDs -> States]
    /\ durableLog \in Seq([coid: COIDs, type: FrameKinds, value: States \union {"n/a"}])
    /\ sendCount \in [COIDs -> Nat]
    /\ exchangeHasOrder \in [COIDs -> BOOLEAN]
    /\ sendAttempted \in [COIDs -> BOOLEAN]
    /\ pendingAppend \in PendingRecs
    /\ fenced \in BOOLEAN
    /\ crashesLeft \in 0..MaxCrashes
    /\ failsLeft \in 0..MaxAppendFailures

Init ==
    /\ state = [c \in COIDs |-> "Intent"]
    /\ durableLog = <<>>
    /\ sendCount = [c \in COIDs |-> 0]
    /\ exchangeHasOrder = [c \in COIDs |-> FALSE]
    /\ sendAttempted = [c \in COIDs |-> FALSE]
    /\ pendingAppend = IdleWriter
    /\ fenced = FALSE
    /\ crashesLeft = MaxCrashes
    /\ failsLeft = MaxAppendFailures

RecordsFor(c) == SelectSeq(durableLog, LAMBDA r: r.coid = c)

(***************************************************************************)
(* Issuing a durable write. The caller is now BLOCKED inside               *)
(* append_durable(); the sink is single-owner-actor, so at most one append *)
(* is outstanding at a time by construction (§9). Modeling this as ONE     *)
(* global pendingAppend rather than one per COID is both faithful and the  *)
(* single biggest state-space saving in the model.                         *)
(***************************************************************************)

StartAppend(c, k, v, n) ==
    /\ CanWrite
    /\ pendingAppend' = [coid |-> c, kind |-> k, value |-> v, next |-> n,
                         landed |-> FALSE, obs |-> "waiting", active |-> TRUE]

\* Gate 9. NOTE: state is NOT transitioned here -- §6.2 puts Intent -> Submitting
\* immediately after Prepared is genuinely .acked(), which is AckAppend below.
StartPrepared(c) ==
    /\ state[c] = "Intent"
    /\ StartAppend(c, "Prepared", "n/a", "Submitting")
    /\ UNCHANGED <<state, durableLog, sendCount, exchangeHasOrder, sendAttempted,
                   fenced, crashesLeft, failsLeft>>

ObservedOutcome(c) == IF exchangeHasOrder[c] THEN "Accepted" ELSE "Rejected"

StartOutcome(c) ==
    /\ state[c] = "Submitting"
    /\ sendCount[c] >= 1
    /\ StartAppend(c, "Outcome", ObservedOutcome(c), ObservedOutcome(c))
    /\ UNCHANGED <<state, durableLog, sendCount, exchangeHasOrder, sendAttempted,
                   fenced, crashesLeft, failsLeft>>

\* L4 §6's reconciliation query: from Ambiguous, ask the exchange directly.
\* Spec round 11 closed the "just re-POST" path entirely -- reconciliation is the
\* only way out of Ambiguous, matching that decision.
StartReconcile(c) ==
    /\ state[c] = "Ambiguous"
    /\ StartAppend(c, "Outcome", ObservedOutcome(c), ObservedOutcome(c))
    /\ UNCHANGED <<state, durableLog, sendCount, exchangeHasOrder, sendAttempted,
                   fenced, crashesLeft, failsLeft>>

(***************************************************************************)
(* The physical write completing -- or not.                                *)
(***************************************************************************)

\* Bytes hit the platter while the caller is still waiting. Normal path.
LandForWaitingCaller ==
    /\ pendingAppend.active
    /\ ~pendingAppend.landed
    /\ pendingAppend.obs = "waiting"
    /\ durableLog' = Append(durableLog, Frame(pendingAppend))
    /\ pendingAppend' = [pendingAppend EXCEPT !.landed = TRUE]
    /\ UNCHANGED <<state, sendCount, exchangeHasOrder, sendAttempted,
                   fenced, crashesLeft, failsLeft>>

\* THE LATE FSYNC. Bytes complete AFTER the caller was told Failed, or after the
\* caller's process died. Nobody observes this happening. Deliberately enabled
\* independently of Recover(c), so TLC explores both "landed before the next
\* process scanned the log" and "landed after it scanned".
LandAfterAbandon ==
    /\ pendingAppend.active
    /\ ~pendingAppend.landed
    /\ pendingAppend.obs = "abandoned"
    /\ durableLog' = Append(durableLog, Frame(pendingAppend))
    /\ pendingAppend' = IdleWriter
    /\ UNCHANGED <<state, sendCount, exchangeHasOrder, sendAttempted,
                   fenced, crashesLeft, failsLeft>>

\* The write genuinely never lands (power cut, device dropped it).
LoseAfterAbandon ==
    /\ pendingAppend.active
    /\ ~pendingAppend.landed
    /\ pendingAppend.obs = "abandoned"
    /\ pendingAppend' = IdleWriter
    /\ UNCHANGED <<state, durableLog, sendCount, exchangeHasOrder, sendAttempted,
                   fenced, crashesLeft, failsLeft>>

(***************************************************************************)
(* What the caller observes. THE ASYMMETRY HERE IS THE POINT.              *)
(***************************************************************************)

\* append_durable() returns Acked. REQUIRES landed: storage is honest about
\* success. The only action that applies a caller-side state transition.
AckAppend ==
    /\ pendingAppend.active
    /\ pendingAppend.landed
    /\ pendingAppend.obs = "waiting"
    /\ state' = [state EXCEPT ![pendingAppend.coid] = pendingAppend.next]
    /\ pendingAppend' = IdleWriter
    /\ UNCHANGED <<durableLog, sendCount, exchangeHasOrder, sendAttempted,
                   fenced, crashesLeft, failsLeft>>

\* append_durable() returns Failed. Deliberately enabled REGARDLESS of `landed` --
\* that is the revision-14 P0: a Failed return never proves the bytes did not
\* land. Never applies pendingAppend.next.
FailAppend ==
    /\ pendingAppend.active
    /\ pendingAppend.obs = "waiting"
    /\ failsLeft > 0
    /\ failsLeft' = failsLeft - 1
    /\ pendingAppend' = IF pendingAppend.landed
                        THEN IdleWriter
                        ELSE [pendingAppend EXCEPT !.obs = "abandoned"]
    /\ IF AbortedPreSendCompensation
       THEN \* PRE-REVISION-14 BUG (regression config only): treat Failed as proof
            \* the operation did not happen -- the old AbortedPreSend compensation.
            \* Releases the registration and does not fence.
            /\ fenced' = FALSE
            /\ state' = [state EXCEPT ![pendingAppend.coid] = "Intent"]
            /\ sendAttempted' = [sendAttempted EXCEPT ![pendingAppend.coid] = FALSE]
       ELSE \* REVISION 14+: fence, leave the COID exactly where it is (§6.4), let
            \* the NEXT process remap it via recovery_scan.
            /\ fenced' = TRUE
            /\ UNCHANGED <<state, sendAttempted>>
    /\ UNCHANGED <<durableLog, sendCount, exchangeHasOrder, crashesLeft>>

(***************************************************************************)
(* Process behavior, crash, restart, and physical reality.                 *)
(***************************************************************************)

Send(c) ==
    /\ ~fenced      \* §6.1.1.1: a fenced L5 stops
    /\ WriterIdle   \* the owner actor is blocked inside append_durable()
    /\ state[c] = "Submitting"
    /\ ~sendAttempted[c]
    /\ sendCount' = [sendCount EXCEPT ![c] = @ + 1]
    /\ sendAttempted' = [sendAttempted EXCEPT ![c] = TRUE]
    /\ UNCHANGED <<state, durableLog, exchangeHasOrder, pendingAppend, fenced,
                   crashesLeft, failsLeft>>

\* Once ANY outcome has been latched for c -- in memory (a pending Outcome write)
\* or on disk -- the packet's fate is settled and the exchange can no longer
\* newly acquire the order. This is exactly the assumption the old atomic
\* AppendOutcome made implicitly; de-atomizing forced it to be written down.
OutcomeLatched(c) ==
    \/ \E i \in 1..Len(durableLog) :
           durableLog[i].coid = c /\ durableLog[i].type = "Outcome"
    \/ /\ pendingAppend.active
       /\ pendingAppend.coid = c
       /\ pendingAppend.kind = "Outcome"

\* An in-flight packet eventually, uncertainly, reaching the exchange. Never
\* firing at all models "the packet was lost" -- TLC explores both branches.
NetworkDeliver(c) ==
    /\ sendCount[c] >= 1
    /\ ~exchangeHasOrder[c]
    /\ ~OutcomeLatched(c)
    /\ exchangeHasOrder' = [exchangeHasOrder EXCEPT ![c] = TRUE]
    /\ UNCHANGED <<state, durableLog, sendCount, sendAttempted, pendingAppend,
                   fenced, crashesLeft, failsLeft>>

\* Volatile memory dies. The QUEUED WRITE DOES NOT -- only its caller does. That
\* is what lets a late fsync survive a crash.
Crash ==
    /\ crashesLeft > 0
    /\ crashesLeft' = crashesLeft - 1
    /\ state' = [c \in COIDs |-> "Uninitialized"]
    /\ sendAttempted' = [c \in COIDs |-> FALSE]
    /\ fenced' = FALSE
    /\ pendingAppend' = IF ~pendingAppend.active \/ pendingAppend.landed
                        THEN IdleWriter
                        ELSE [pendingAppend EXCEPT !.obs = "abandoned"]
    /\ UNCHANGED <<durableLog, sendCount, exchangeHasOrder, failsLeft>>

\* §6.1.1.1: recovery after operator intervention starts from a clean process.
\* Same EFFECT as Crash but a DISTINCT ACTION: it does not consume the crash
\* budget, and it is the one thing we declare weakly fair. Without it, `fenced`
\* is a legitimate permanent block and EventualResolution would report a FALSE
\* violation. Declaring WF here is precisely the statement "we assume operators
\* eventually respond to an escalation" -- the honest assumption, and what
\* separates a real deadlock finding from a spurious one.
OperatorRepairRestart ==
    /\ fenced
    /\ state' = [c \in COIDs |-> "Uninitialized"]
    /\ sendAttempted' = [c \in COIDs |-> FALSE]
    /\ fenced' = FALSE
    /\ pendingAppend' = IF ~pendingAppend.active \/ pendingAppend.landed
                        THEN IdleWriter
                        ELSE [pendingAppend EXCEPT !.obs = "abandoned"]
    /\ UNCHANGED <<durableLog, sendCount, exchangeHasOrder, crashesLeft, failsLeft>>

\* recovery_scan()'s core rule (spec §6.1.4): the LAST durable record for a coid
\* determines its recovered state. A Prepared-only frame recovers as Ambiguous
\* under the correct design -- toggled to the historical bug via the constant.
\* Deliberately NOT guarded by WriterIdle: a late fsync racing recovery_scan is a
\* real scenario, and this is where the model expresses it.
RecoveredState(c) ==
    LET recs == RecordsFor(c) IN
    IF Len(recs) = 0 THEN "Intent"
    ELSE LET last == recs[Len(recs)] IN
         IF last.type = "Outcome" THEN last.value
         ELSE IF RecoverAmbiguousCorrectly THEN "Ambiguous" ELSE "Submitting"

Recover(c) ==
    /\ state[c] = "Uninitialized"
    /\ state' = [state EXCEPT ![c] = RecoveredState(c)]
    /\ UNCHANGED <<durableLog, sendCount, exchangeHasOrder, sendAttempted,
                   pendingAppend, fenced, crashesLeft, failsLeft>>

Next ==
    \/ \E c \in COIDs : StartPrepared(c)
    \/ \E c \in COIDs : StartOutcome(c)
    \/ \E c \in COIDs : StartReconcile(c)
    \/ LandForWaitingCaller
    \/ LandAfterAbandon
    \/ LoseAfterAbandon
    \/ AckAppend
    \/ FailAppend
    \/ \E c \in COIDs : Send(c)
    \/ \E c \in COIDs : NetworkDeliver(c)
    \/ \E c \in COIDs : Recover(c)
    \/ Crash
    \/ OperatorRepairRestart

Spec == Init /\ [][Next]_vars

\* Model-checking bound. MaxLogLen was raised from 6 to 9 for this revision: a
\* failed-but-landed append now produces an orphan frame the old bound didn't
\* have to accommodate.
\* sendCount stays at <= 3, NOT <= 1 -- a bound of 1 would make NoDoubleSend
\* unfalsifiable by construction and every run would pass vacuously.
\* NOTE: the liveness config must NOT use this (see the cfg comments).
StateConstraint ==
    /\ Len(durableLog) <= MaxLogLen
    /\ \A c \in COIDs : sendCount[c] <= 3

(***************************************************************************)
(* Safety invariants -- a violation here means the DESIGN is wrong.         *)
(***************************************************************************)

\* The headline property: the real network POST is invoked at most once per
\* client-order-id, ever, across any number of crashes. This is what round 11's
\* "Gate 8 is structurally the only order-placing send site, no re-POST path
\* exists" actually claims.
NoDoubleSend == \A c \in COIDs : sendCount[c] <= 1

\* The actual Gate-9 claim: no send without a durable Prepared frame first.
\* Replaces the old NoBlindResendWhenAlreadyPrepared -- see header note (3).
SendImpliesDurablePrepared ==
    \A c \in COIDs :
        sendCount[c] >= 1 =>
            \E i \in 1..Len(durableLog) :
                durableLog[i].coid = c /\ durableLog[i].type = "Prepared"

\* Idempotent replay (spec round 8's "first event establishes, same-state is a
\* no-op" discipline): a terminal in-memory state always agrees with the log.
TerminalIsStable ==
    \A c \in COIDs :
        state[c] \in FinalStates =>
            LET recs == RecordsFor(c) IN
            /\ Len(recs) >= 1
            /\ recs[Len(recs)].type = "Outcome"
            /\ recs[Len(recs)].value = state[c]

\* §6.1.1.1 / §6.4: a fenced process issues no further durable writes. The only
\* pending write a fenced process may have is the one that fenced it.
FencedWriterMakesNoNewWrites ==
    fenced => (~pendingAppend.active \/ pendingAppend.obs = "abandoned")

(***************************************************************************)
(* Modeling-sanity invariants -- a violation here means the MODEL is wrong, *)
(* not the design. These exist so a future edit to the model cannot quietly *)
(* make the whole exercise vacuous.                                         *)
(***************************************************************************)

\* Catches the latch-vs-delivery hole that the previous revision actually had.
OutcomeMatchesGroundTruth ==
    \A i \in 1..Len(durableLog) :
        durableLog[i].type = "Outcome" =>
            ((durableLog[i].value = "Accepted") = exchangeHasOrder[durableLog[i].coid])

\* Reality is only ever created by a real send. Catches a mis-edited
\* NetworkDeliver guard.
ExchangeOnlyHasSentOrders ==
    \A c \in COIDs : exchangeHasOrder[c] => sendCount[c] >= 1

\* Structural well-formedness of the new machinery: catches a dropped EXCEPT or
\* a mis-threaded `next` field anywhere on the Start*/Ack path.
PendingWellFormed ==
    pendingAppend.active =>
        /\ pendingAppend.landed => pendingAppend.obs = "waiting"
        /\ pendingAppend.kind = "Prepared" => pendingAppend.next = "Submitting"
        /\ pendingAppend.kind = "Outcome" => pendingAppend.next = pendingAppend.value

(***************************************************************************)
(* Reachability probe -- NOT an invariant of anything. Run in its own cfg;  *)
(* TLC MUST report a violation. state[c] = "Intent" alongside a durable      *)
(* Prepared frame is the unique signature of "bytes landed, caller was told  *)
(* Failed". If TLC does NOT violate this, the late-fsync extension is        *)
(* decorative and nothing above proves anything about it.                    *)
(***************************************************************************)
LateFsyncUnreachable ==
    ~ \E i \in 1..Len(durableLog) :
        /\ durableLog[i].type = "Prepared"
        /\ state[durableLog[i].coid] = "Intent"

(***************************************************************************)
(* Liveness. Progress is measured against the DURABLE log, not `state`:     *)
(* `state` is volatile, so a crash one step after reaching Accepted wipes    *)
(* it and any property phrased over `state` is either trivially weak or      *)
(* outright false. Resolved(c) is also stable -- once the last frame for c   *)
(* is an Outcome, RecoveredState(c) never returns "Intent"/"Ambiguous"       *)
(* again, so no further frame for c can be appended.                         *)
(***************************************************************************)

Resolved(c) ==
    LET recs == RecordsFor(c) IN
    Len(recs) >= 1 /\ recs[Len(recs)].type = "Outcome"

EventualResolution == \A c \in COIDs : <>Resolved(c)

Fairness ==
    \* Per-c, never \E -- WF_vars(\E c : A(c)) only promises SOME c progresses.
    /\ \A c \in COIDs : WF_vars(StartPrepared(c))
    /\ \A c \in COIDs : WF_vars(StartOutcome(c))
    /\ \A c \in COIDs : WF_vars(StartReconcile(c))
    /\ \A c \in COIDs : WF_vars(Send(c))
    /\ \A c \in COIDs : WF_vars(Recover(c))
    \* Storage does its job when it is working.
    /\ WF_vars(LandForWaitingCaller)
    /\ WF_vars(AckAppend)
    \* MANDATORY: an abandoned-but-unresolved write pins WriterIdle FALSE
    \* forever, disabling every Start*/Send action. Fairness must be on the
    \* DISJUNCTION so both "late fsync lands" and "bytes truly lost" stay
    \* reachable; WF on each disjunct separately would be wrong.
    /\ WF_vars(LandAfterAbandon \/ LoseAfterAbandon)
    /\ WF_vars(OperatorRepairRestart)
    \* Deliberately NO fairness on Crash, FailAppend, or NetworkDeliver -- we
    \* never assume a crash happens, that storage fails, or that a packet
    \* arrives. All three are bounded instead (Crash/FailAppend by their
    \* budgets, NetworkDeliver by OutcomeLatched), which is what makes the
    \* state graph finite WITHOUT a CONSTRAINT -- and that is what makes this
    \* liveness check sound.

SpecLive == Init /\ [][Next]_vars /\ Fairness

====
