---------------------------- MODULE notsent_closure ----------------------------
(* Q23 slice 1 (docs/NOTSENT_CLOSURE_DESIGN.md): the final send claim and the NotSent closure.
 *
 * WHAT THIS MODELS. The window between "the Prepared frame has been durably ACKed" (Gate 12d,
 * live_submit_orchestrator.hpp) and "the first byte of the order request may leave the process"
 * (the submit_port.call() a few lines below it), which task 1A.6 (blueprint 5.9.2) fills with a final
 * authorization gate, and the durable frame (NotSent) that lets an order which was refused in that window
 * be closed WITHOUT being treated as "possibly on the wire". The claims it checks:
 *
 *   NotSentImpliesNeverCrossed  a durable NotSent frame for an order means no send for it ever happened.
 *   ClosedNotSentIsTrue         the same for the in-memory state, including the one recovery produces
 *                               (ClosedNotSentIsDurable: and that state always has a durable frame behind it).
 *   ReleaseNeedsProof           nothing is released (in-flight slot, reservation) without a durable closing
 *                               frame -- NotSent or an exchange Outcome.
 *   NoClaimUnderRevoke          an order is never claimed for sending while a revocation is latched.
 *   SendRequiresClaim / ClaimRejectExclusive   the only way to cross is a claim, and the decision is exclusive.
 *   NoDoubleSend / SendImpliesDurablePrepared   the two properties durable_log_recovery.tla proves for the
 *                               old design, re-proved with the new frame and the new gate in play.
 *   ReplayLegal                 per order, the durable frame kinds always form a sequence the replay accepts
 *                               (a closing frame is last; nothing contradicts NotSent).
 *
 * WHAT IT IS NOT. It is a model of the PROTOCOL, not of the code: nothing here is implemented. It does not
 * model the frame layout or format versions (the v5/v6 rules are in the design document and are tested when
 * they are implemented), the log/anchor/directory layers behind "landed" (one boolean, as in
 * durable_log_recovery.tla), more than one process at a time (host fencing is task 6.7), or an asynchronous
 * writer with request ids and slot generations (PERF-01 -- the contract for it is in the design document).
 *
 * CONVENTIONS (the same as durable_log_recovery.tla, which this module reuses on purpose)
 *   Volatile (wiped by Crash): state, decision, seen, revoked, fenced.
 *   Durable (survives Crash):  durableLog.
 *   Ghost (no process reads it): sendCount, exchangeHasOrder, released, hist (per order: was it ever claimed,
 *                               ever rejected, and was a revocation latched at the claim step), the `landed`
 *                               flag inside pendingAppend and the `late` flag inside a frame.
 *   An append is not atomic: AckAppend requires the bytes to have landed, FailAppend does not require that
 *   they did not (the late fsync). That asymmetry is the whole reason a Failed append can never be read as
 *   "nothing happened".
 *
 * THE EIGHT SWITCHES (TRUE = the design). A config that turns one off MUST fail, with the invariant named in
 * its file; that is what keeps the model from proving nothing. A ninth constant, ReconcileConcludesAbsent, is an
 * ASSUMPTION about the environment and is described after them.
 *
 *   AtomicClaim                 TRUE: "is a revocation latched?" and "claim the order" are one step.
 *                               FALSE: the check is a separate earlier step, the claim trusts its answer
 *                               (the "seven independent loads and then a plain call" shape of 5.9.2).
 *   ExclusiveDecision           TRUE: once the decision left Undecided nothing overwrites it.
 *                               FALSE: a revoker with a plain store can turn SendClaimed/Sent into Rejecting.
 *   FinalGateAfterPrepared      TRUE: the decision exists only after the Prepared ACK.
 *                               FALSE: the claim can be taken before it (the gate is in the wrong place).
 *   NoCompensationOnFailure     TRUE: a Failed append fences the writer and changes nothing else.
 *                               FALSE: the revision-13 AbortedPreSend compensation -- "Failed means it did
 *                               not happen" -- releases and rewinds the order.
 *   ReleaseAfterAck             TRUE: release happens when the closing frame is ACKed (or recovered).
 *                               FALSE: release happens when the closing append is merely issued.
 *   RecoveryNeverClosesPrepared TRUE: a Prepared frame with nothing after it recovers as Ambiguous.
 *                               FALSE: it recovers as NotSent ("probably never sent").
 *   SingleAttemptCoid           TRUE: a client-order-id is one attempt; once it is closed with NotSent nothing
 *                               starts it again. FALSE: a closed order goes back to Intent and is prepared and
 *                               sent under the same id, behind its own NotSent frame.
 *   RecoveryWaitsForWriter      TRUE: a successor's recovery scan starts only after the predecessor's last
 *                               write has landed or been lost -- what the exclusive log lock gives. This is
 *                               an ASSUMPTION of the design, and the config with FALSE shows exactly what
 *                               depends on it (ReplayLegal; the safety claims above do not).
 *
 * THE OTHER ASSUMPTION: ReconcileConcludesAbsent. TRUE: reconciling an Ambiguous order the exchange does not
 * have ends in an Outcome (Rejected). FALSE: reconciliation can only ever conclude "the exchange HAS it" --
 * which is what the tree does today: query_order() / parse_order_query_response() fold every answer that is not
 * a positive match, an "unknown order" error included, into QueryOutcome::Inconclusive, so a never-sent order
 * with a Prepared frame stays Ambiguous until the R-10 quarantine escalates it to the operator. The safety
 * claims do not depend on it; EventualSettlement does, and the FALSE config shows which orders it strands.
 *
 * KNOWN RESOLUTION LIMITS
 *   - An order that recovers with no durable frame is `Dead`: nothing resumes an attempt, a new attempt has a
 *     new client-order-id (Gate 9). durable_log_recovery.tla lets such an order start again; here that would
 *     let a late NotSent land in front of a new Prepared for the SAME id, a sequence the real system cannot
 *     produce and the contract forbids (an id is single-attempt).
 *   - The Intent frame is not modeled. An order with no frame is "Intent" in memory; the frame matters for
 *     orphan accounting (SAFE-07), not for whether an order crossed.
 *   - `revoked` is one latch per incarnation. It stands for every reason the final gate can refuse (shutdown
 *     event, generation change, breaker latch, expired clock token, expired authorization, lost reservation).
 *)
EXTENDS Naturals, Sequences, TLC

CONSTANTS
    AtomicClaim,
    ExclusiveDecision,
    FinalGateAfterPrepared,
    NoCompensationOnFailure,
    ReleaseAfterAck,
    SingleAttemptCoid,
    RecoveryNeverClosesPrepared,
    RecoveryWaitsForWriter,
    ReconcileConcludesAbsent,
    MaxCrashes,          \* modeling budget, NOT a design bound
    MaxAppendFailures,   \* modeling budget, NOT a design bound
    MaxLogLen            \* StateConstraint bound; a CONSTANT so the saturation run is a cfg change

ASSUME AtomicClaim                  \in BOOLEAN
ASSUME ExclusiveDecision            \in BOOLEAN
ASSUME FinalGateAfterPrepared       \in BOOLEAN
ASSUME NoCompensationOnFailure      \in BOOLEAN
ASSUME ReleaseAfterAck              \in BOOLEAN
ASSUME SingleAttemptCoid            \in BOOLEAN
ASSUME RecoveryNeverClosesPrepared  \in BOOLEAN
ASSUME RecoveryWaitsForWriter       \in BOOLEAN
ASSUME ReconcileConcludesAbsent     \in BOOLEAN
ASSUME MaxCrashes \in Nat
ASSUME MaxAppendFailures \in Nat
ASSUME MaxLogLen \in Nat

COIDs == {1, 2}

OrderStates == {"Intent", "Submitting", "Ambiguous", "Accepted", "Rejected", "NotSent", "Dead", "Uninitialized"}
Decisions   == {"None", "Undecided", "SendClaimed", "Sent", "Rejecting"}
SeenValues  == {"none", "clear", "revoked"}
FrameKinds  == {"Prepared", "Outcome", "NotSent"}
FrameValues == {"Accepted", "Rejected", "n/a"}

VARIABLES
    state,               \* [COIDs -> OrderStates]            volatile
    decision,            \* [COIDs -> Decisions]              volatile: the final gate's decision word
    seen,                \* [COIDs -> SeenValues]             volatile: a stale observation (AtomicClaim = FALSE only)
    revoked,             \* BOOLEAN                           volatile: a revocation condition is latched
    durableLog,          \* Seq of frames                     durable, append-only
    sendCount,           \* [COIDs -> Nat]                    ghost: how many times the request may have left
    exchangeHasOrder,    \* [COIDs -> BOOLEAN]                ghost: ground truth at the exchange
    pendingAppend,       \* the single outstanding durable write (see durable_log_recovery.tla)
    fenced,              \* BOOLEAN                           volatile: the writer fence (spec 6.1.1.1)
    crashesLeft,
    failsLeft,
    hist,                \* [COIDs -> [claimed, rejected, underRevoke]]  ghost: what the decision word ever was, and
                         \* whether a revocation was latched at the claim step
    released             \* [COIDs -> BOOLEAN]                ghost: were the order's resources released?

vars == <<state, decision, seen, revoked, durableLog, sendCount, exchangeHasOrder, pendingAppend, fenced,
          crashesLeft, failsLeft, hist, released>>

PendingRecs ==
    [coid   : COIDs,
     kind   : FrameKinds,
     value  : FrameValues,
     landed : BOOLEAN,
     obs    : {"waiting", "abandoned"},
     active : BOOLEAN]

IdleWriter == [coid |-> 1, kind |-> "Prepared", value |-> "n/a",
               landed |-> FALSE, obs |-> "waiting", active |-> FALSE]

Frame(p, late) == [coid |-> p.coid, type |-> p.kind, value |-> p.value, late |-> late]
WriterIdle == ~pendingAppend.active
CanWrite   == WriterIdle /\ ~fenced

TypeOK ==
    /\ state \in [COIDs -> OrderStates]
    /\ decision \in [COIDs -> Decisions]
    /\ seen \in [COIDs -> SeenValues]
    /\ revoked \in BOOLEAN
    /\ durableLog \in Seq([coid: COIDs, type: FrameKinds, value: FrameValues, late: BOOLEAN])
    /\ sendCount \in [COIDs -> Nat]
    /\ exchangeHasOrder \in [COIDs -> BOOLEAN]
    /\ pendingAppend \in PendingRecs
    /\ fenced \in BOOLEAN
    /\ crashesLeft \in 0..MaxCrashes
    /\ failsLeft \in 0..MaxAppendFailures
    /\ hist \in [COIDs -> [claimed: BOOLEAN, rejected: BOOLEAN, underRevoke: BOOLEAN]]
    /\ released \in [COIDs -> BOOLEAN]

Init ==
    /\ state = [c \in COIDs |-> "Intent"]
    /\ decision = [c \in COIDs |-> "None"]
    /\ seen = [c \in COIDs |-> "none"]
    /\ revoked = FALSE
    /\ durableLog = <<>>
    /\ sendCount = [c \in COIDs |-> 0]
    /\ exchangeHasOrder = [c \in COIDs |-> FALSE]
    /\ pendingAppend = IdleWriter
    /\ fenced = FALSE
    /\ crashesLeft = MaxCrashes
    /\ failsLeft = MaxAppendFailures
    /\ hist = [c \in COIDs |-> [claimed |-> FALSE, rejected |-> FALSE, underRevoke |-> FALSE]]
    /\ released = [c \in COIDs |-> FALSE]

RecordsFor(c) == SelectSeq(durableLog, LAMBDA r: r.coid = c)
DurableHas(c, k) == \E i \in 1..Len(durableLog) : durableLog[i].coid = c /\ durableLog[i].type = k

(***************************************************************************)
(* Issuing a durable write. The caller is blocked inside append_durable();  *)
(* at most one append is outstanding (the single-owner sink).               *)
(***************************************************************************)

StartAppend(c, k, v) ==
    /\ CanWrite
    /\ pendingAppend' = [coid |-> c, kind |-> k, value |-> v, landed |-> FALSE, obs |-> "waiting", active |-> TRUE]

\* Releasing "at issue" is the ReleaseAfterAck = FALSE defect.
ReleaseAtIssue(c) == IF ReleaseAfterAck THEN released ELSE [released EXCEPT ![c] = TRUE]

\* Gate 12d: the Prepared frame. The state moves on the ACK (AckAppend), never here.
StartPrepared(c) ==
    /\ state[c] = "Intent"
    /\ decision[c] = "None"
    /\ StartAppend(c, "Prepared", "n/a")
    /\ UNCHANGED <<state, decision, seen, revoked, durableLog, sendCount, exchangeHasOrder, fenced,
                   crashesLeft, failsLeft, hist, released>>

\* A refusal before the Prepared frame (Gates 11 to 12c): the order is closed with the same frame, so it does
\* not remain an Intent-only orphan. It cannot have crossed: crossing needs a durable Prepared first.
StartNotSentPre(c) ==
    /\ state[c] = "Intent"
    /\ decision[c] = "None"
    /\ StartAppend(c, "NotSent", "n/a")
    /\ released' = ReleaseAtIssue(c)
    /\ UNCHANGED <<state, decision, seen, revoked, durableLog, sendCount, exchangeHasOrder, fenced,
                   crashesLeft, failsLeft, hist>>

\* The final gate refused (or a revoker won the decision): close the order. Only the incarnation whose
\* decision word says Rejecting writes this frame, and in that incarnation Rejecting excludes SendClaimed.
StartNotSent(c) ==
    /\ state[c] = "Submitting"
    /\ decision[c] = "Rejecting"
    /\ StartAppend(c, "NotSent", "n/a")
    /\ released' = ReleaseAtIssue(c)
    /\ UNCHANGED <<state, decision, seen, revoked, durableLog, sendCount, exchangeHasOrder, fenced,
                   crashesLeft, failsLeft, hist>>

ObservedOutcome(c) == IF exchangeHasOrder[c] THEN "Accepted" ELSE "Rejected"

StartOutcome(c) ==
    /\ state[c] = "Submitting"
    /\ decision[c] = "Sent"
    /\ sendCount[c] >= 1
    /\ StartAppend(c, "Outcome", ObservedOutcome(c))
    /\ UNCHANGED <<state, decision, seen, revoked, durableLog, sendCount, exchangeHasOrder, fenced,
                   crashesLeft, failsLeft, hist, released>>

\* Reconciliation of an Ambiguous order (the only way out of Ambiguous, spec round 11).
StartReconcile(c) ==
    /\ state[c] = "Ambiguous"
    /\ (ReconcileConcludesAbsent \/ exchangeHasOrder[c])
    /\ StartAppend(c, "Outcome", ObservedOutcome(c))
    /\ UNCHANGED <<state, decision, seen, revoked, durableLog, sendCount, exchangeHasOrder, fenced,
                   crashesLeft, failsLeft, hist, released>>

(***************************************************************************)
(* The physical write completing -- or not.                                *)
(***************************************************************************)

LandForWaitingCaller ==
    /\ pendingAppend.active
    /\ ~pendingAppend.landed
    /\ pendingAppend.obs = "waiting"
    /\ durableLog' = Append(durableLog, Frame(pendingAppend, FALSE))
    /\ pendingAppend' = [pendingAppend EXCEPT !.landed = TRUE]
    /\ UNCHANGED <<state, decision, seen, revoked, sendCount, exchangeHasOrder, fenced,
                   crashesLeft, failsLeft, hist, released>>

\* THE LATE FSYNC: bytes complete after the caller was told Failed, or after the caller's process died.
LandAfterAbandon ==
    /\ pendingAppend.active
    /\ ~pendingAppend.landed
    /\ pendingAppend.obs = "abandoned"
    /\ durableLog' = Append(durableLog, Frame(pendingAppend, TRUE))
    /\ pendingAppend' = IdleWriter
    /\ UNCHANGED <<state, decision, seen, revoked, sendCount, exchangeHasOrder, fenced,
                   crashesLeft, failsLeft, hist, released>>

LoseAfterAbandon ==
    /\ pendingAppend.active
    /\ ~pendingAppend.landed
    /\ pendingAppend.obs = "abandoned"
    /\ pendingAppend' = IdleWriter
    /\ UNCHANGED <<state, decision, seen, revoked, durableLog, sendCount, exchangeHasOrder, fenced,
                   crashesLeft, failsLeft, hist, released>>

(***************************************************************************)
(* What the caller observes.                                               *)
(***************************************************************************)

\* append_durable() returned Acked. REQUIRES landed. The only action that applies a caller-side transition.
AckAppend ==
    /\ pendingAppend.active
    /\ pendingAppend.landed
    /\ pendingAppend.obs = "waiting"
    /\ LET c == pendingAppend.coid IN
       IF pendingAppend.kind = "Prepared"
       THEN /\ state' = [state EXCEPT ![c] = "Submitting"]
            /\ decision' = [decision EXCEPT ![c] = IF @ = "None" THEN "Undecided" ELSE @]
            /\ seen' = [seen EXCEPT ![c] = "none"]
            /\ released' = released
       ELSE /\ state' = [state EXCEPT ![c] = IF pendingAppend.kind = "NotSent"
                                                THEN (IF SingleAttemptCoid THEN "NotSent" ELSE "Intent")
                                                ELSE pendingAppend.value]
            /\ decision' = [decision EXCEPT ![c] = IF pendingAppend.kind = "NotSent" /\ ~SingleAttemptCoid
                                                   THEN "None" ELSE @]
            /\ released' = [released EXCEPT ![c] = TRUE]
            /\ UNCHANGED seen
    /\ pendingAppend' = IdleWriter
    /\ UNCHANGED <<revoked, durableLog, sendCount, exchangeHasOrder, fenced, crashesLeft, failsLeft,
                   hist>>

\* append_durable() returned Failed. Enabled whether or not the bytes landed; never applies anything.
FailAppend ==
    /\ pendingAppend.active
    /\ pendingAppend.obs = "waiting"
    /\ failsLeft > 0
    /\ failsLeft' = failsLeft - 1
    /\ pendingAppend' = IF pendingAppend.landed
                        THEN IdleWriter
                        ELSE [pendingAppend EXCEPT !.obs = "abandoned"]
    /\ IF NoCompensationOnFailure
       THEN \* Spec 6.4 / revision 14: fence, leave the order exactly where it is, let recovery decide.
            /\ fenced' = TRUE
            /\ UNCHANGED <<state, decision, seen, released>>
       ELSE \* Revision 13's AbortedPreSend, reintroduced on purpose: Failed is read as "did not happen".
            /\ fenced' = FALSE
            /\ state' = [state EXCEPT ![pendingAppend.coid] = "Intent"]
            /\ decision' = [decision EXCEPT ![pendingAppend.coid] = "None"]
            /\ seen' = [seen EXCEPT ![pendingAppend.coid] = "none"]
            /\ released' = [released EXCEPT ![pendingAppend.coid] = TRUE]
    /\ UNCHANGED <<revoked, durableLog, sendCount, exchangeHasOrder, crashesLeft, hist>>

(***************************************************************************)
(* The final gate: one decision word per order, taken from Undecided once.  *)
(***************************************************************************)

\* Something the final gate refuses on becomes true (shutdown event, generation change, breaker latch, expired
\* token or authorization, lost reservation). Nothing recalls bytes already sent.
Revoke ==
    /\ ~revoked
    /\ revoked' = TRUE
    /\ UNCHANGED <<state, decision, seen, durableLog, sendCount, exchangeHasOrder, pendingAppend, fenced,
                   crashesLeft, failsLeft, hist, released>>

\* Where a claim may be taken. The design: only an order whose Prepared frame is ACKed and which is Undecided.
ClaimWindow(c) ==
    IF FinalGateAfterPrepared
    THEN state[c] = "Submitting" /\ decision[c] = "Undecided"
    ELSE state[c] \in {"Intent", "Submitting"} /\ decision[c] \in {"None", "Undecided"}

\* AtomicClaim = TRUE: the revocation test and the claim are one step (a compare-and-swap on the decision
\* word that also fails once a revocation is latched).
Claim(c) ==
    /\ AtomicClaim
    /\ ~fenced
    /\ WriterIdle
    /\ ClaimWindow(c)
    /\ ~revoked
    /\ decision' = [decision EXCEPT ![c] = "SendClaimed"]
    /\ hist' = [hist EXCEPT ![c] = [@ EXCEPT !.claimed = TRUE, !.underRevoke = revoked]]
    /\ UNCHANGED <<state, seen, revoked, durableLog, sendCount, exchangeHasOrder, pendingAppend, fenced,
                   crashesLeft, failsLeft, released>>

\* AtomicClaim = FALSE: the test is a step of its own ...
ObserveRevoked(c) ==
    /\ ~AtomicClaim
    /\ ~fenced
    /\ WriterIdle
    /\ ClaimWindow(c)
    /\ seen' = [seen EXCEPT ![c] = IF revoked THEN "revoked" ELSE "clear"]
    /\ UNCHANGED <<state, decision, revoked, durableLog, sendCount, exchangeHasOrder, pendingAppend, fenced,
                   crashesLeft, failsLeft, hist, released>>

\* ... and the claim trusts it. `hist` records what the world looked like at the claim step.
ClaimStale(c) ==
    /\ ~AtomicClaim
    /\ ~fenced
    /\ WriterIdle
    /\ ClaimWindow(c)
    /\ seen[c] = "clear"
    /\ decision' = [decision EXCEPT ![c] = "SendClaimed"]
    /\ hist' = [hist EXCEPT ![c] = [@ EXCEPT !.claimed = TRUE, !.underRevoke = revoked]]
    /\ UNCHANGED <<state, seen, revoked, durableLog, sendCount, exchangeHasOrder, pendingAppend, fenced,
                   crashesLeft, failsLeft, released>>

\* The owner (a failing check) or a revoker (a compare-and-swap) takes the decision "do not send". With
\* ExclusiveDecision = FALSE a plain store may also overwrite a claim that was already taken.
Reject(c) ==
    /\ ~fenced
    /\ state[c] = "Submitting"
    /\ \/ decision[c] = "Undecided"
       \/ /\ ~ExclusiveDecision
          /\ decision[c] \in {"SendClaimed", "Sent"}
    /\ decision' = [decision EXCEPT ![c] = "Rejecting"]
    /\ hist' = [hist EXCEPT ![c].rejected = TRUE]
    /\ UNCHANGED <<state, seen, revoked, durableLog, sendCount, exchangeHasOrder, pendingAppend, fenced,
                   crashesLeft, failsLeft, released>>

(***************************************************************************)
(* Process behavior, crash, restart, and physical reality.                 *)
(***************************************************************************)

\* The point of no return: after this the request may be on the wire.
Send(c) ==
    /\ ~fenced
    /\ WriterIdle
    /\ decision[c] = "SendClaimed"
    /\ sendCount' = [sendCount EXCEPT ![c] = @ + 1]
    /\ decision' = [decision EXCEPT ![c] = "Sent"]
    /\ UNCHANGED <<state, seen, revoked, durableLog, exchangeHasOrder, pendingAppend, fenced,
                   crashesLeft, failsLeft, hist, released>>

\* Once an Outcome is latched (in memory or on disk) the exchange can no longer newly acquire the order
\* (the request's recvWindow has long passed; durable_log_recovery.tla makes the same assumption).
OutcomeLatched(c) ==
    \/ DurableHas(c, "Outcome")
    \/ /\ pendingAppend.active
       /\ pendingAppend.coid = c
       /\ pendingAppend.kind = "Outcome"

NetworkDeliver(c) ==
    /\ sendCount[c] >= 1
    /\ ~exchangeHasOrder[c]
    /\ ~OutcomeLatched(c)
    /\ exchangeHasOrder' = [exchangeHasOrder EXCEPT ![c] = TRUE]
    /\ UNCHANGED <<state, decision, seen, revoked, durableLog, sendCount, pendingAppend, fenced,
                   crashesLeft, failsLeft, hist, released>>

\* Volatile memory dies. The queued write does not -- only its caller does.
Crash ==
    /\ crashesLeft > 0
    /\ crashesLeft' = crashesLeft - 1
    /\ state' = [c \in COIDs |-> "Uninitialized"]
    /\ decision' = [c \in COIDs |-> "None"]
    /\ seen' = [c \in COIDs |-> "none"]
    /\ revoked' = FALSE
    /\ fenced' = FALSE
    /\ pendingAppend' = IF ~pendingAppend.active \/ pendingAppend.landed
                        THEN IdleWriter
                        ELSE [pendingAppend EXCEPT !.obs = "abandoned"]
    /\ UNCHANGED <<durableLog, sendCount, exchangeHasOrder, failsLeft, hist, released>>

\* Operator repair after a fence: the same effect as a crash without spending the crash budget.
OperatorRepairRestart ==
    /\ fenced
    /\ state' = [c \in COIDs |-> "Uninitialized"]
    /\ decision' = [c \in COIDs |-> "None"]
    /\ seen' = [c \in COIDs |-> "none"]
    /\ revoked' = FALSE
    /\ fenced' = FALSE
    /\ pendingAppend' = IF ~pendingAppend.active \/ pendingAppend.landed
                        THEN IdleWriter
                        ELSE [pendingAppend EXCEPT !.obs = "abandoned"]
    /\ UNCHANGED <<durableLog, sendCount, exchangeHasOrder, crashesLeft, failsLeft, hist, released>>

\* recovery_scan(): the LAST durable frame for an order decides its state. Prepared with nothing after it is
\* Ambiguous -- the order may have crossed and nothing here can prove it did not. No frame at all: the attempt
\* is Dead (see KNOWN RESOLUTION LIMITS).
RecoveredState(c) ==
    LET recs == RecordsFor(c) IN
    IF Len(recs) = 0 THEN "Dead"
    ELSE LET last == recs[Len(recs)] IN
         IF last.type = "Outcome" THEN last.value
         ELSE IF last.type = "NotSent" THEN "NotSent"
         ELSE IF RecoveryNeverClosesPrepared THEN "Ambiguous" ELSE "NotSent"

Recover(c) ==
    /\ state[c] = "Uninitialized"
    /\ (~RecoveryWaitsForWriter \/ WriterIdle)
    /\ state' = [state EXCEPT ![c] = IF RecoveredState(c) = "NotSent" /\ ~SingleAttemptCoid
                                     THEN "Intent" ELSE RecoveredState(c)]
    /\ released' = [released EXCEPT ![c] = IF RecoveredState(c) \in {"NotSent", "Accepted", "Rejected"} THEN TRUE ELSE @]
    /\ UNCHANGED <<decision, seen, revoked, durableLog, sendCount, exchangeHasOrder, pendingAppend, fenced,
                   crashesLeft, failsLeft, hist>>

Next ==
    \/ \E c \in COIDs : StartPrepared(c)
    \/ \E c \in COIDs : StartNotSentPre(c)
    \/ \E c \in COIDs : StartNotSent(c)
    \/ \E c \in COIDs : StartOutcome(c)
    \/ \E c \in COIDs : StartReconcile(c)
    \/ LandForWaitingCaller
    \/ LandAfterAbandon
    \/ LoseAfterAbandon
    \/ AckAppend
    \/ FailAppend
    \/ Revoke
    \/ \E c \in COIDs : Claim(c)
    \/ \E c \in COIDs : ObserveRevoked(c)
    \/ \E c \in COIDs : ClaimStale(c)
    \/ \E c \in COIDs : Reject(c)
    \/ \E c \in COIDs : Send(c)
    \/ \E c \in COIDs : NetworkDeliver(c)
    \/ \E c \in COIDs : Recover(c)
    \/ Crash
    \/ OperatorRepairRestart

Spec == Init /\ [][Next]_vars

\* sendCount stays at <= 3, NOT <= 1: a bound of 1 would make NoDoubleSend unfalsifiable by construction.
\* The liveness config must NOT use this (see durable_log_recovery.tla's cfg comments).
StateConstraint ==
    /\ Len(durableLog) <= MaxLogLen
    /\ \A c \in COIDs : sendCount[c] <= 3

(***************************************************************************)
(* Safety invariants -- a violation here means the DESIGN is wrong.         *)
(***************************************************************************)

NoDoubleSend == \A c \in COIDs : sendCount[c] <= 1

SendImpliesDurablePrepared == \A c \in COIDs : sendCount[c] >= 1 => DurableHas(c, "Prepared")

\* The new claim. The NotSent frame is a durable statement "this order never crossed"; the only things that
\* may write it are an incarnation whose decision word says Rejecting (so it never claimed) and, for an order
\* with no Prepared frame at all, an incarnation that never got to the gate.
NotSentImpliesNeverCrossed == \A c \in COIDs : DurableHas(c, "NotSent") => sendCount[c] = 0

\* The same for memory, including the state recovery hands out.
ClosedNotSentIsTrue == \A c \in COIDs : state[c] = "NotSent" => sendCount[c] = 0

ClosedNotSentIsDurable == \A c \in COIDs : state[c] = "NotSent" => DurableHas(c, "NotSent")

\* Resources are released only against a durable closing frame: NotSent, or an exchange Outcome.
ReleaseNeedsProof ==
    \A c \in COIDs : released[c] => (DurableHas(c, "NotSent") \/ DurableHas(c, "Outcome"))

NoClaimUnderRevoke == \A c \in COIDs : ~hist[c].underRevoke

\* The only way to cross is a claim ...
SendRequiresClaim == \A c \in COIDs : sendCount[c] >= 1 => hist[c].claimed

\* ... and the decision is exclusive: one order never both claimed for sending and rejected.
ClaimRejectExclusive == \A c \in COIDs : ~(hist[c].claimed /\ hist[c].rejected)

\* Per order, the durable frame kinds follow what replay accepts: a closing frame is last, an Outcome is
\* preceded by Prepared, NotSent is either the only frame or follows exactly one Prepared.
KindSeq(c) == [i \in 1..Len(RecordsFor(c)) |-> RecordsFor(c)[i].type]

LegalKindSeq(s) ==
    \/ s = <<>>
    \/ s = <<"NotSent">>
    \/ s = <<"Prepared">>
    \/ s = <<"Prepared", "NotSent">>
    \/ /\ Len(s) >= 2
       /\ s[1] = "Prepared"
       /\ \A i \in 2..Len(s) : s[i] = "Outcome"

ReplayLegal == \A c \in COIDs : LegalKindSeq(KindSeq(c))

\* Idempotent replay: a terminal in-memory state agrees with the log's last frame.
TerminalIsStable ==
    \A c \in COIDs :
        state[c] \in {"Accepted", "Rejected"} =>
            LET recs == RecordsFor(c) IN
            /\ Len(recs) >= 1
            /\ recs[Len(recs)].type = "Outcome"
            /\ recs[Len(recs)].value = state[c]

\* A fenced process issues no further durable writes; the only pending write it may have is the one that
\* fenced it.
FencedWriterMakesNoNewWrites ==
    fenced => (~pendingAppend.active \/ pendingAppend.obs = "abandoned")

(***************************************************************************)
(* Modeling-sanity invariants -- a violation means the MODEL is wrong.      *)
(***************************************************************************)

OutcomeMatchesGroundTruth ==
    \A i \in 1..Len(durableLog) :
        durableLog[i].type = "Outcome" =>
            ((durableLog[i].value = "Accepted") = exchangeHasOrder[durableLog[i].coid])

ExchangeOnlyHasSentOrders == \A c \in COIDs : exchangeHasOrder[c] => sendCount[c] >= 1

PendingWellFormed ==
    pendingAppend.active =>
        /\ pendingAppend.landed => pendingAppend.obs = "waiting"
        /\ pendingAppend.kind = "Outcome" => pendingAppend.value \in {"Accepted", "Rejected"}
        /\ pendingAppend.kind \in {"Prepared", "NotSent"} => pendingAppend.value = "n/a"

(***************************************************************************)
(* Reachability probes -- NOT invariants of anything. Each runs in its own  *)
(* cfg and TLC MUST report a violation: it shows the scenario the main      *)
(* config's "no error" depends on is actually explored.                     *)
(***************************************************************************)

\* A NotSent frame can become durable at all.
NotSentNeverDurable == ~ \E i \in 1..Len(durableLog) : durableLog[i].type = "NotSent"

\* ... including by landing after the caller was told Failed or died (the late fsync).
LateNotSentNeverLands == ~ \E i \in 1..Len(durableLog) : durableLog[i].type = "NotSent" /\ durableLog[i].late

\* An order can be on the wire while a revocation is latched: claim first, revoke second -- the design
\* promises no recall, only that a revocation which came first wins.
SentWhileRevokedNeverHappens == ~ \E c \in COIDs : sendCount[c] >= 1 /\ revoked

(***************************************************************************)
(* Liveness. Progress is measured against the DURABLE log, as in            *)
(* durable_log_recovery.tla, never against volatile `state`.                *)
(***************************************************************************)

Resolved(c) ==
    LET recs == RecordsFor(c) IN
    Len(recs) >= 1 /\ recs[Len(recs)].type \in {"Outcome", "NotSent"}

\* An attempt that left no frame is Dead: nothing remains to close (it is an orphan only if an Intent frame
\* exists, which this model does not carry).
Settled(c) == Resolved(c) \/ state[c] = "Dead"

EventualSettlement == \A c \in COIDs : <>Settled(c)

Fairness ==
    /\ \A c \in COIDs : WF_vars(StartPrepared(c))
    /\ \A c \in COIDs : WF_vars(Claim(c) \/ Reject(c))
    /\ \A c \in COIDs : WF_vars(StartNotSent(c))
    /\ \A c \in COIDs : WF_vars(Send(c))
    /\ \A c \in COIDs : WF_vars(StartOutcome(c))
    /\ \A c \in COIDs : WF_vars(StartReconcile(c))
    /\ \A c \in COIDs : WF_vars(Recover(c))
    /\ WF_vars(LandForWaitingCaller)
    /\ WF_vars(AckAppend)
    \* The disjunction, not each disjunct: both "the late write lands" and "it is lost" must stay reachable.
    /\ WF_vars(LandAfterAbandon \/ LoseAfterAbandon)
    /\ WF_vars(OperatorRepairRestart)
    \* Deliberately NO fairness on Crash, FailAppend, Revoke, NetworkDeliver or StartNotSentPre: nothing is
    \* assumed to happen. Crash and FailAppend are bounded by their budgets, Revoke by its latch.

SpecLive == Init /\ [][Next]_vars /\ Fairness

====
