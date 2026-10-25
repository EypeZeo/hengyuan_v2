---------------------------- MODULE operator_resolution ----------------------------
(* D5 (docs/OPERATOR_RESOLUTION_DESIGN.md): the operator's way back for an order this system can no longer resolve by itself.
 *
 * THE SITUATION. An order has a durable Prepared frame and nothing after it. The process that wrote it is gone (crash, or the
 * operator stopped it), the request may or may not have left, and reconciliation can never answer "the exchange does not have
 * it" (docs/NOTSENT_CLOSURE_DESIGN.md section 1 point 2): the order stays unresolved, the process degrades at every start, and the
 * log cannot be edited. The way out is a tool the operator runs on the stopped host: it takes the log's exclusive lock, proves that
 * the exchange has no such order and never will, and appends ONE closing frame (OpClosed below; the event of the SUBMITPORT spec
 * 6.5 item 4, OrderOperatorResolved).
 *
 * WHAT MAKES "NEVER WILL" PROVABLE. A signed request is accepted only while  serverTime - timestamp <= recvWindow  (checked on
 * arrival and again before the matching engine; Binance Spot REST, "Timing security"). The process that signed it is dead before the
 * tool holds the lock, so after `Window` ticks (recvWindow plus the bound on the process's clock offset) no request of that process
 * can still be accepted. A read of the exchange after that moment cannot be fooled by a request still in flight. A read may also
 * trail the exchange's true state (`Lag` ticks; allOrders is served from a database), so the evidence must be read at least `Lag`
 * ticks after the fence: an "absent" answer then describes a state that is already past the last moment an order could appear.
 *
 * CLAIMS (safety)
 *   ClosedAbsentIsTrue   a durable OpClosed frame means the exchange never accepted the request -- before the frame or after it.
 *   ReplayLegal          per order the durable frames are a sequence the replay accepts: Prepared, then at most one closing frame.
 *   ExclusiveHolds       (design only) the tool and a live process never hold the log together.
 *   OutcomeOnlyIfDelivered   (sanity) reconciliation closes only what the exchange has -- the assumption stated below.
 * CLAIM (progress)       EventualSettlement: every prepared order is eventually closed. Under these fairness assumptions and no others:
 *                        time advances; reconciliation matches what it can match; a Degraded process is stopped; the tool, once locked, reads,
 *                        closes and unlocks; the operator walks away from a run only a bounded number of times (MaxAborts); and a dead process
 *                        he is not holding down is restarted (STRONG fairness -- he cannot keep stopping and holding it for ever).
 *                        Without the last two the claim is false for ANY design, the first counterexample of this model was exactly that.
 *
 * TIME IS RELATIVE AND SATURATING. No global clock: each quantity that matters is a counter that stops at its cap -- the age of a
 * signed request (cap Window+1 = expired), the age of an acceptance (cap Lag+1 = every read sees it), the age of the tool's lock. That
 * keeps the graph finite without a horizon, so the liveness run needs no CONSTRAINT and no run can "run out of time".
 *
 * THE SWITCHES (TRUE = the design). A config that turns one off MUST fail, with the invariant named in its file.
 *   ExclusiveOpen       the tool can take the log only when the process is down and held down by the operator, and no process starts
 *                       while the tool holds it (durable_log_store.hpp: no-sharing CreateFileA / flock LOCK_EX). FALSE: the tool can
 *                       run beside a live process.
 *   FenceFromLock       the fence starts at the tool's own lock, measured by the tool. FALSE: it starts at a crash time the operator
 *                       CLAIMS (anything up to a full window ago).
 *   FenceChecked        evidence is accepted only after the fence. FALSE: right away.
 *   LagAllowance        the evidence read must also be Lag ticks past the fence. FALSE: the fence alone.
 *   ChecksExchange      the tool reads the exchange and refuses an order it can see. FALSE: the operator's word is enough.
 * TWO ASSUMPTIONS ABOUT THE ENVIRONMENT (named, with a config pair each, like notsent_closure.tla):
 *   OperatorPathExists          TRUE: the tool exists. FALSE: the tree today -- nothing can close an order the exchange never saw.
 *   ReconcileResolvesExisting   TRUE: reconciliation matches an order the exchange has and closes it with an Outcome frame.
 *                               FALSE: it cannot (a response that fails validation, an archived order). The tool refuses such an
 *                               order too -- its scope is orders that provably left no position -- so they stay open. The config
 *                               with FALSE shows what v1 does not cover.
 *
 * NOT IN THE MODEL. Frame layout and versions (the design document); the non-atomic append and the late fsync (notsent_closure.tla;
 * here an append is one step, and a lost closing append just leaves the order unresolved for a second run of the tool); the point /
 * list / open-orders reads as separate queries (they are one read here -- the model's only imperfection of a read is its lag); fills
 * (v1 closes only orders that left no position); the order's symbol; a second sender of the same credentials (host fencing, 6.7).
 *)
EXTENDS Naturals, Sequences, TLC

CONSTANTS
    ExclusiveOpen,
    FenceFromLock,
    FenceChecked,
    LagAllowance,
    ChecksExchange,
    OperatorPathExists,
    ReconcileResolvesExisting,
    Window,        \* ticks a signed request stays acceptable: recvWindow plus the clock-offset bound
    Lag,           \* ticks a read of the exchange may trail its true state
    MaxCrashes,    \* modeling budget, NOT a design bound
    MaxAborts      \* modeling budget: how often the operator may walk away from a run of the tool

ASSUME ExclusiveOpen              \in BOOLEAN
ASSUME FenceFromLock              \in BOOLEAN
ASSUME FenceChecked               \in BOOLEAN
ASSUME LagAllowance               \in BOOLEAN
ASSUME ChecksExchange             \in BOOLEAN
ASSUME OperatorPathExists         \in BOOLEAN
ASSUME ReconcileResolvesExisting  \in BOOLEAN
ASSUME Window \in Nat /\ Window >= 1
ASSUME Lag \in Nat
ASSUME MaxCrashes \in Nat
ASSUME MaxAborts \in Nat

COIDs   == {1, 2}
LockCap == Window + Lag + 1

FrameKinds == {"Prepared", "Outcome", "OpClosed"}
Evidence   == {"none", "absentFresh", "absentStale", "present"}

VARIABLES
    procAlive,     \* BOOLEAN             a trading process holds the log
    holdDown,      \* BOOLEAN             the operator stopped the process and keeps it down while the tool works
    canSend,       \* [COIDs -> BOOLEAN]  volatile: this incarnation prepared c and has not sent it yet
    durableLog,    \* Seq of [coid, type] append-only
    sent,          \* [COIDs -> BOOLEAN]  ghost: a request for c was signed and left
    age,           \* [COIDs -> 0..Window+1]  ghost: ticks since signing; Window+1 = no longer acceptable
    delivered,     \* [COIDs -> BOOLEAN]  ghost: the exchange accepted the request
    dAge,          \* [COIDs -> 0..Lag+1] ghost: ticks since acceptance; Lag+1 = every read sees it
    locked,        \* BOOLEAN             the tool holds the log
    lockAge,       \* 0..LockCap          ticks since the tool took the log
    offset,        \* 0..Window           how long ago the crash was, as CLAIMED at lock time (0 unless FenceFromLock = FALSE)
    ev,            \* [COIDs -> Evidence] what the tool's last read of the exchange said about c
    crashesLeft,
    abortsLeft

vars == <<procAlive, holdDown, canSend, durableLog, sent, age, delivered, dAge, locked, lockAge, offset, ev, crashesLeft, abortsLeft>>

TypeOK ==
    /\ procAlive \in BOOLEAN
    /\ holdDown \in BOOLEAN
    /\ canSend \in [COIDs -> BOOLEAN]
    /\ durableLog \in Seq([coid: COIDs, type: FrameKinds])
    /\ sent \in [COIDs -> BOOLEAN]
    /\ age \in [COIDs -> 0..(Window + 1)]
    /\ delivered \in [COIDs -> BOOLEAN]
    /\ dAge \in [COIDs -> 0..(Lag + 1)]
    /\ locked \in BOOLEAN
    /\ lockAge \in 0..LockCap
    /\ offset \in 0..Window
    /\ ev \in [COIDs -> Evidence]
    /\ crashesLeft \in 0..MaxCrashes
    /\ abortsLeft \in 0..MaxAborts

Init ==
    /\ procAlive = TRUE
    /\ holdDown = FALSE
    /\ canSend = [c \in COIDs |-> FALSE]
    /\ durableLog = <<>>
    /\ sent = [c \in COIDs |-> FALSE]
    /\ age = [c \in COIDs |-> 0]
    /\ delivered = [c \in COIDs |-> FALSE]
    /\ dAge = [c \in COIDs |-> 0]
    /\ locked = FALSE
    /\ lockAge = 0
    /\ offset = 0
    /\ ev = [c \in COIDs |-> "none"]
    /\ crashesLeft = MaxCrashes
    /\ abortsLeft = MaxAborts

DurableHas(c, k) == \E i \in 1..Len(durableLog) : durableLog[i].coid = c /\ durableLog[i].type = k
Prepared(c)   == DurableHas(c, "Prepared")
Closed(c)     == DurableHas(c, "Outcome") \/ DurableHas(c, "OpClosed")
Unresolved(c) == Prepared(c) /\ ~Closed(c)

\* Reconciliation can find an order the exchange has. (It never concludes "absent": that is the tree today.)
Matchable(c) == ReconcileResolvesExisting /\ delivered[c]

\* A running process that has recovered an order it can neither send nor match: R-10 escalated it, the start-up reconcile timed out, and the
\* process is Degraded for good. The operator reacts to exactly this. (The R-10 timer is NOT tied to the request window: the operator may
\* well act while a request is still acceptable -- that is what the fence is for.)
Degraded ==
    /\ procAlive
    /\ \E c \in COIDs : Unresolved(c)
    /\ \A c \in COIDs : Unresolved(c) => (~canSend[c] /\ ~Matchable(c))

(***************************************************************************)
(* The trading process and the exchange.                                   *)
(***************************************************************************)

\* Gate 12d: the Prepared frame. The process may now send.
ProcPrepare(c) ==
    /\ procAlive
    /\ ~Prepared(c)
    /\ durableLog' = Append(durableLog, [coid |-> c, type |-> "Prepared"])
    /\ canSend' = [canSend EXCEPT ![c] = TRUE]
    /\ UNCHANGED <<procAlive, holdDown, sent, age, delivered, dAge, locked, lockAge, offset, ev, crashesLeft, abortsLeft>>

\* The request is signed and leaves. After this the exchange may accept it, but only while it is fresh.
ProcSend(c) ==
    /\ procAlive
    /\ canSend[c]
    /\ ~sent[c]
    /\ sent' = [sent EXCEPT ![c] = TRUE]
    /\ age' = [age EXCEPT ![c] = 0]
    /\ canSend' = [canSend EXCEPT ![c] = FALSE]
    /\ UNCHANGED <<procAlive, holdDown, durableLog, delivered, dAge, locked, lockAge, offset, ev, crashesLeft, abortsLeft>>

\* The exchange accepts the request -- or never does (the packet is lost: no fairness on this action).
Deliver(c) ==
    /\ sent[c]
    /\ ~delivered[c]
    /\ age[c] <= Window          \* the acceptance rule: a stale request is rejected, on arrival and again before the engine
    /\ delivered' = [delivered EXCEPT ![c] = TRUE]
    /\ dAge' = [dAge EXCEPT ![c] = 0]
    /\ UNCHANGED <<procAlive, holdDown, canSend, durableLog, sent, age, locked, lockAge, offset, ev, crashesLeft, abortsLeft>>

Tick ==
    /\ age' = [c \in COIDs |-> IF sent[c] /\ age[c] <= Window THEN age[c] + 1 ELSE age[c]]
    /\ dAge' = [c \in COIDs |-> IF delivered[c] /\ dAge[c] <= Lag THEN dAge[c] + 1 ELSE dAge[c]]
    /\ lockAge' = IF locked /\ lockAge < LockCap THEN lockAge + 1 ELSE lockAge
    /\ UNCHANGED <<procAlive, holdDown, canSend, durableLog, sent, delivered, locked, offset, ev, crashesLeft, abortsLeft>>

Crash ==
    /\ procAlive
    /\ crashesLeft > 0
    /\ crashesLeft' = crashesLeft - 1
    /\ procAlive' = FALSE
    /\ canSend' = [c \in COIDs |-> FALSE]
    /\ UNCHANGED <<holdDown, durableLog, sent, age, delivered, dAge, locked, lockAge, offset, ev, abortsLeft>>

\* The operator stops a Degraded process and keeps it down while he works.
OpStop ==
    /\ Degraded
    /\ procAlive' = FALSE
    /\ holdDown' = TRUE
    /\ canSend' = [c \in COIDs |-> FALSE]
    /\ UNCHANGED <<durableLog, sent, age, delivered, dAge, locked, lockAge, offset, ev, crashesLeft, abortsLeft>>

\* The process is already dead (a crash) and the operator decides to keep it that way and work. No fairness: his choice.
OpHold ==
    /\ ~procAlive
    /\ ~holdDown
    /\ holdDown' = TRUE
    /\ UNCHANGED <<procAlive, canSend, durableLog, sent, age, delivered, dAge, locked, lockAge, offset, ev, crashesLeft, abortsLeft>>

\* A process starts: recovery reads the log, so an order with a Prepared frame and no closing frame is unresolved again.
ProcStart ==
    /\ ~procAlive
    /\ ~holdDown
    /\ (ExclusiveOpen => ~locked)
    /\ procAlive' = TRUE
    /\ UNCHANGED <<holdDown, canSend, durableLog, sent, age, delivered, dAge, locked, lockAge, offset, ev, crashesLeft, abortsLeft>>

\* Reconciliation matches an order the exchange has and closes it with an Outcome frame.
ReconcileFound(c) ==
    /\ procAlive
    /\ Unresolved(c)
    /\ Matchable(c)
    /\ durableLog' = Append(durableLog, [coid |-> c, type |-> "Outcome"])
    /\ UNCHANGED <<procAlive, holdDown, canSend, sent, age, delivered, dAge, locked, lockAge, offset, ev, crashesLeft, abortsLeft>>

(***************************************************************************)
(* The operator's tool.                                                    *)
(***************************************************************************)

\* The fence has passed for a read taken now: the acceptance window of the dead process is over, and the read cannot trail the
\* exchange's state back to a moment before it.
FenceOk == ~FenceChecked \/ (lockAge + offset >= Window + (IF LagAllowance THEN Lag ELSE 0))

ToolLock ==
    /\ OperatorPathExists
    /\ ~locked
    /\ (ExclusiveOpen => holdDown)       \* the process is down and stays down while the tool works (holdDown implies ~procAlive)
    /\ \E c \in COIDs : Unresolved(c)
    /\ locked' = TRUE
    /\ lockAge' = 0
    /\ \E o \in (IF FenceFromLock THEN {0} ELSE 0..Window) : offset' = o
    /\ ev' = [c \in COIDs |-> "none"]
    /\ UNCHANGED <<procAlive, holdDown, canSend, durableLog, sent, age, delivered, dAge, crashesLeft, abortsLeft>>

\* One read of the exchange for c. It reflects the exchange as it was `s` ticks ago, for some s in 0..Lag -- the tool cannot choose.
ToolRead(c) ==
    /\ locked
    /\ Unresolved(c)
    /\ \E s \in 0..Lag :
         LET sees   == delivered[c] /\ dAge[c] >= s
             absent == IF ChecksExchange THEN ~sees ELSE TRUE
         IN ev' = [ev EXCEPT ![c] = IF ~absent THEN "present" ELSE IF FenceOk THEN "absentFresh" ELSE "absentStale"]
    /\ UNCHANGED <<procAlive, holdDown, canSend, durableLog, sent, age, delivered, dAge, locked, lockAge, offset, crashesLeft, abortsLeft>>

\* The closing frame: appended only after a read that was past the fence and found nothing.
ToolClose(c) ==
    /\ locked
    /\ Unresolved(c)
    /\ ev[c] = "absentFresh"
    /\ durableLog' = Append(durableLog, [coid |-> c, type |-> "OpClosed"])
    /\ ev' = [ev EXCEPT ![c] = "none"]
    /\ UNCHANGED <<procAlive, holdDown, canSend, sent, age, delivered, dAge, locked, lockAge, offset, crashesLeft, abortsLeft>>

\* The tool is done when every order still unresolved is one the exchange has: reconciliation closes those after the restart.
ToolUnlock ==
    /\ locked
    /\ \A c \in COIDs : Unresolved(c) => ev[c] = "present"
    /\ locked' = FALSE
    /\ lockAge' = 0
    /\ holdDown' = FALSE
    /\ ev' = [c \in COIDs |-> "none"]
    /\ UNCHANGED <<procAlive, canSend, durableLog, sent, age, delivered, dAge, offset, crashesLeft, abortsLeft>>

\* The operator walks away from a run of the tool (a budget, like crashes: nothing is assumed about his patience, but not even an
\* operator is assumed to give up for ever -- progress is claimed for an operator who finishes a run).
ToolAbort ==
    /\ locked
    /\ abortsLeft > 0
    /\ abortsLeft' = abortsLeft - 1
    /\ locked' = FALSE
    /\ lockAge' = 0
    /\ holdDown' = FALSE
    /\ ev' = [c \in COIDs |-> "none"]
    /\ UNCHANGED <<procAlive, canSend, durableLog, sent, age, delivered, dAge, offset, crashesLeft>>

Next ==
    \/ \E c \in COIDs : ProcPrepare(c)
    \/ \E c \in COIDs : ProcSend(c)
    \/ \E c \in COIDs : Deliver(c)
    \/ Tick
    \/ Crash
    \/ OpStop
    \/ OpHold
    \/ ProcStart
    \/ \E c \in COIDs : ReconcileFound(c)
    \/ ToolLock
    \/ \E c \in COIDs : ToolRead(c)
    \/ \E c \in COIDs : ToolClose(c)
    \/ ToolUnlock
    \/ ToolAbort

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* Safety -- a violation means the DESIGN is wrong.                         *)
(***************************************************************************)

\* The headline. A closing frame is a durable statement "the exchange does not have this order"; it must stay true for ever, so
\* a request that is accepted AFTER the frame is as bad as one accepted before it.
ClosedAbsentIsTrue == \A c \in COIDs : DurableHas(c, "OpClosed") => ~delivered[c]

\* Per order the durable frames are: nothing; Prepared; Prepared and exactly one closing frame.
KindSeq(c) == [i \in 1..Len(SelectSeq(durableLog, LAMBDA r: r.coid = c)) |->
                 SelectSeq(durableLog, LAMBDA r: r.coid = c)[i].type]

LegalKindSeq(s) ==
    \/ s = <<>>
    \/ s = <<"Prepared">>
    \/ s = <<"Prepared", "Outcome">>
    \/ s = <<"Prepared", "OpClosed">>

ReplayLegal == \A c \in COIDs : LegalKindSeq(KindSeq(c))

\* The design's mutual exclusion: the tool and a live process never hold the log together.
ExclusiveHolds == ExclusiveOpen => ~(locked /\ procAlive)

(***************************************************************************)
(* Modeling-sanity invariants -- a violation means the MODEL is wrong.      *)
(***************************************************************************)

\* The exchange accepts only what was sent, and only while the request was fresh.
DeliveredOnlyIfSent == \A c \in COIDs : delivered[c] => sent[c]

\* A process sends only what it prepared.
SentOnlyIfPrepared == \A c \in COIDs : sent[c] => Prepared(c)

\* Reconciliation closes only what the exchange HAS: it never concludes "absent" (the tree today -- the assumption this model makes
\* explicit; notsent_closure.tla carries the same one as ReconcileConcludesAbsent = FALSE). Without this invariant nothing would
\* notice a reconcile that did.
OutcomeOnlyIfDelivered == \A c \in COIDs : DurableHas(c, "Outcome") => delivered[c]

(***************************************************************************)
(* Reachability probes -- NOT invariants of anything. Each runs in its own  *)
(* cfg and TLC MUST report a violation: the scenario the main config's "no  *)
(* error" depends on is really explored.                                    *)
(***************************************************************************)

\* The tool can close an order at all.
OpClosedNeverDurable == ~ \E c \in COIDs : DurableHas(c, "OpClosed")

\* ... including one whose request DID leave and was never accepted (the lost packet): the case the fence exists for.
ClosedSentUndeliveredNeverHappens == ~ \E c \in COIDs : DurableHas(c, "OpClosed") /\ sent[c]

\* The tool can face an order the exchange has, and say so.
PresentVerdictNeverHappens == ~ \E c \in COIDs : ev[c] = "present"

(***************************************************************************)
(* Progress. Measured against the DURABLE log, never against volatile state *)
(***************************************************************************)

EventualSettlement == \A c \in COIDs : (Prepared(c) ~> Closed(c))

Fairness ==
    /\ WF_vars(Tick)
    /\ \A c \in COIDs : WF_vars(ProcSend(c))
    /\ WF_vars(OpStop)
    /\ SF_vars(ProcStart)     \* STRONG: a dead process the operator is not holding down gets restarted -- he cannot hold it down for ever
    /\ \A c \in COIDs : WF_vars(ReconcileFound(c))
    /\ WF_vars(ToolLock)
    /\ \A c \in COIDs : WF_vars(ToolRead(c))
    /\ \A c \in COIDs : WF_vars(ToolClose(c))
    /\ WF_vars(ToolUnlock)
    \* Deliberately NO fairness on ProcPrepare, Deliver, Crash, OpHold or ToolAbort: nothing is assumed to happen -- the world may
    \* prepare nothing, lose a packet, crash (bounded by its budget), the operator may leave a dead process alone and walk away
    \* from a run of the tool (bounded by its budget).

SpecLive == Init /\ [][Next]_vars /\ Fairness

====
