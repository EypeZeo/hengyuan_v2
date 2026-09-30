-------------------------- MODULE inflight_lifecycle --------------------------
(***************************************************************************)
(* The InFlightRegistry slot lifecycle: orchestrate_submit() ->            *)
(* ToReconcileRing -> poll_once() -> ReconcileEventRing ->                 *)
(* drain_reconcile_events() -> slot release.                              *)
(*                                                                         *)
(* WHY THIS MODEL EXISTS (audit FORMAL-GAP-017).                          *)
(*                                                                         *)
(* formal/README.md's Scope section lists what durable_log_recovery.tla   *)
(* deliberately does not cover. The audit that produced this file found   *)
(* real defects in three subsystems, and all three were on that list --   *)
(* while the two subsystems that DO have models came through the same     *)
(* review clean. That correlation is the argument for this file.          *)
(*                                                                         *)
(* The defect this models is EXEC-INFLIGHT-003: an order that the         *)
(* exchange ACCEPTED kept its InFlightRegistry slot (correctly -- it is   *)
(* resting on the book and a blind resubmit must stay blocked) but was    *)
(* never handed to anything that could later discover it had filled or    *)
(* been cancelled. drain_reconcile_events() releases a slot only on       *)
(* is_exchange_final(), so the slot became unreleasable for the life of   *)
(* the process; 64 of them fail-closed every subsequent submit.           *)
(*                                                                         *)
(* SCOPE. Deliberately abstract about everything that is not the slot     *)
(* lifecycle: no durable log, no crash/recovery, no key material, no      *)
(* wall-clock or backoff. Those either have their own model               *)
(* (durable_log_recovery.tla) or are genuinely out of this property's     *)
(* resolution. Modeling them here without that depth would produce        *)
(* something that looks rigorous while encoding guesses -- the same       *)
(* discipline formal/README.md's own Scope note already states.          *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets, Sequences

CONSTANTS
    COIDs,          \* the client-order-ids this run may submit
    MaxSlots,       \* InFlightRegistry capacity (kMaxInFlight)
    MaxQueryAttempts,   \* abstracts the UNKNOWN quarantine criterion: the number of SENT
                        \* queries after which an Ambiguous order escalates. The code used
                        \* to hard-code 3 (OrderRecord::kMaxQueryAttempts); since R-10
                        \* (Owner decision 2026-09-29) the criterion is "5 sent queries and
                        \* 5000 ms, or a 15000 ms hard cap" (UnknownQuarantinePolicy in
                        \* order_lifecycle.hpp). This model has no time, so it keeps only
                        \* the count guard; the hard cap can escalate an order with fewer
                        \* sent queries, a path this model does not explore.
    TrackAcceptedOrders \* MODEL SWITCH, see inflight_lifecycle_prefix_bug.cfg:
                        \*   TRUE  = current design (Accepted enters the reconcile loop)
                        \*   FALSE = the pre-fix behaviour, which MUST violate
                        \*           NoLiveOrderIsUntracked

VARIABLES
    state,       \* [COIDs -> OrderState] -- "Unsubmitted" before Submit
    slots,       \* SUBSET COIDs currently holding an InFlightRegistry slot
    inbound,     \* Seq(COIDs)  -- ToReconcileRing, hot thread -> reconcile thread
    tracked,     \* SUBSET COIDs currently in OrderTracker
    outbound,    \* Seq(COIDs)  -- ReconcileEventRing, reconcile thread -> hot thread
    attempts     \* [COIDs -> Nat] -- OrderRecord::query_attempts (SENT queries only since R-10)

vars == <<state, slots, inbound, tracked, outbound, attempts>>

(***************************************************************************)
(* OrderState, mirroring order_lifecycle.hpp. "Unsubmitted" is a modeling  *)
(* device for "orchestrate_submit() has not run for this coid yet"; it has *)
(* no C++ counterpart because the record does not exist yet.               *)
(***************************************************************************)
ExchangeFinal == {"Filled", "Cancelled", "Rejected", "Expired"}
Live          == {"Accepted", "PartialFill"}
OrderStates   == {"Unsubmitted", "Ambiguous", "EscalatedToOperator"}
                 \cup Live \cup ExchangeFinal

(***************************************************************************)
(* Legal query targets, mirroring detail::is_legal_query_target().         *)
(***************************************************************************)
AmbiguousTargets == Live \cup ExchangeFinal
\* Rejected is excluded on purpose: Binance's REJECTED is a submit-time status,
\* never produced for an order that already reached NEW (see validate_transition()'s
\* own note). Ambiguous -> Rejected covers the real case and stays in
\* AmbiguousTargets above.
ExchangeFinalFromLive == ExchangeFinal \ {"Rejected"}
LiveTargets(s) ==
    IF s = "Accepted" THEN ExchangeFinalFromLive \cup {"PartialFill"}
                      ELSE ExchangeFinalFromLive   \* from PartialFill

TypeOK ==
    /\ state \in [COIDs -> OrderStates]
    /\ slots \subseteq COIDs
    /\ tracked \subseteq COIDs
    /\ attempts \in [COIDs -> 0..MaxQueryAttempts]
    /\ \A i \in 1..Len(inbound)  : inbound[i]  \in COIDs
    /\ \A i \in 1..Len(outbound) : outbound[i] \in COIDs

Init ==
    /\ state    = [c \in COIDs |-> "Unsubmitted"]
    /\ slots    = {}
    /\ inbound  = << >>
    /\ tracked  = {}
    /\ outbound = << >>
    /\ attempts = [c \in COIDs |-> 0]

(***************************************************************************)
(* orchestrate_submit(). Gate F4 refuses when the registry is at capacity  *)
(* -- that is the fail-closed behaviour, and the point of the invariants   *)
(* below is that it should not be reachable through slot LEAKAGE.          *)
(***************************************************************************)
SubmitRejected(c) ==
    /\ state[c] = "Unsubmitted"
    /\ Cardinality(slots) < MaxSlots
    /\ state' = [state EXCEPT ![c] = "Rejected"]
    /\ UNCHANGED <<slots, inbound, tracked, outbound, attempts>>
       \* acquires then synchronously releases (live_submit_orchestrator.hpp:650)

SubmitAccepted(c) ==
    /\ state[c] = "Unsubmitted"
    /\ Cardinality(slots) < MaxSlots
    /\ state' = [state EXCEPT ![c] = "Accepted"]
    /\ slots' = slots \cup {c}
    /\ inbound' = IF TrackAcceptedOrders THEN Append(inbound, c) ELSE inbound
    /\ UNCHANGED <<tracked, outbound, attempts>>

SubmitAmbiguous(c) ==
    /\ state[c] = "Unsubmitted"
    /\ Cardinality(slots) < MaxSlots
    /\ state' = [state EXCEPT ![c] = "Ambiguous"]
    /\ slots' = slots \cup {c}
    /\ inbound' = Append(inbound, c)
    /\ UNCHANGED <<tracked, outbound, attempts>>

(***************************************************************************)
(* poll_once(): drain inbound into the tracker.                            *)
(***************************************************************************)
Track ==
    /\ Len(inbound) > 0
    /\ tracked' = tracked \cup {Head(inbound)}
    /\ inbound' = Tail(inbound)
    /\ UNCHANGED <<state, slots, outbound, attempts>>

(***************************************************************************)
(* poll_once(): query a tracked order.                                     *)
(*                                                                         *)
(* An untracked order is deliberately NOT queryable -- that is exactly the *)
(* stranding this model is about.                                          *)
(***************************************************************************)
QueryAmbiguousResolves(c, target) ==
    /\ c \in tracked
    /\ state[c] = "Ambiguous"
    /\ attempts[c] < MaxQueryAttempts
    /\ target \in AmbiguousTargets
    /\ attempts' = [attempts EXCEPT ![c] = @ + 1]
    /\ state' = [state EXCEPT ![c] = target]
    /\ outbound' = Append(outbound, c)
       \* untrack only when there is nothing left to discover
    /\ tracked' = IF target \in ExchangeFinal THEN tracked \ {c} ELSE tracked
    /\ UNCHANGED <<slots, inbound>>

QueryAmbiguousInconclusive(c) ==
    /\ c \in tracked
    /\ state[c] = "Ambiguous"
    /\ attempts[c] < MaxQueryAttempts
    /\ attempts' = [attempts EXCEPT ![c] = @ + 1]
    /\ UNCHANGED <<state, slots, inbound, tracked, outbound>>

Escalate(c) ==
    /\ c \in tracked
    /\ state[c] = "Ambiguous"
    /\ attempts[c] >= MaxQueryAttempts
    /\ state' = [state EXCEPT ![c] = "EscalatedToOperator"]
    /\ outbound' = Append(outbound, c)
    /\ tracked' = tracked \ {c}
    /\ UNCHANGED <<slots, inbound, attempts>>

QueryLiveResolves(c, target) ==
    /\ c \in tracked
    /\ state[c] \in Live
    /\ target \in LiveTargets(state[c])
    /\ state' = [state EXCEPT ![c] = target]
    /\ outbound' = Append(outbound, c)
    /\ tracked' = IF target \in ExchangeFinal THEN tracked \ {c} ELSE tracked
    /\ UNCHANGED <<slots, inbound, attempts>>
       \* attempts deliberately NOT incremented for a live order: query_attempts is
       \* the Ambiguous escalation cap, and a resting order polled a few times must
       \* not escalate itself.

QueryLiveUnchanged(c) ==
    /\ c \in tracked
    /\ state[c] \in Live
    /\ UNCHANGED vars   \* "still resting": no transition, no event, no release

(***************************************************************************)
(* drain_reconcile_events(): release the slot IFF exchange-final.           *)
(***************************************************************************)
Drain ==
    /\ Len(outbound) > 0
    /\ LET c == Head(outbound) IN
        /\ slots' = IF state[c] \in ExchangeFinal THEN slots \ {c} ELSE slots
        /\ outbound' = Tail(outbound)
    /\ UNCHANGED <<state, inbound, tracked, attempts>>

Next ==
    \/ \E c \in COIDs : SubmitRejected(c)
    \/ \E c \in COIDs : SubmitAccepted(c)
    \/ \E c \in COIDs : SubmitAmbiguous(c)
    \/ Track
    \/ \E c \in COIDs, t \in AmbiguousTargets : QueryAmbiguousResolves(c, t)
    \/ \E c \in COIDs : QueryAmbiguousInconclusive(c)
    \/ \E c \in COIDs : Escalate(c)
    \/ \E c \in COIDs, t \in ExchangeFinal \cup Live : QueryLiveResolves(c, t)
    \/ \E c \in COIDs : QueryLiveUnchanged(c)
    \/ Drain

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* SAFETY                                                                  *)
(***************************************************************************)

(* Nobody holds a slot who never submitted. *)
NoPhantomSlot == \A c \in slots : state[c] # "Unsubmitted"

(* THE INVARIANT THE DEFECT VIOLATED.                                      *)
(*                                                                         *)
(* An order that still holds a slot and is neither exchange-final nor      *)
(* operator-owned MUST still be reachable by something that can advance    *)
(* it: queued for the tracker, in the tracker, or already carrying an      *)
(* event to the drain. Otherwise its slot can never be released and the    *)
(* order can never reach a terminal state -- protocol section 31's         *)
(* "accepted request must eventually reach a terminal state".              *)
InFlightSeq(c) == \E i \in 1..Len(inbound)  : inbound[i]  = c
InEventSeq(c)  == \E i \in 1..Len(outbound) : outbound[i] = c

NoLiveOrderIsUntracked ==
    \A c \in slots :
        (state[c] \in Live \cup {"Ambiguous"})
            => (c \in tracked \/ InFlightSeq(c) \/ InEventSeq(c))

(* A slot is released exactly when the order is exchange-final and its     *)
(* event has been drained. Escalated orders KEEP their slot on purpose --  *)
(* they may still be live on the exchange, awaiting operator resolution.   *)
TerminalOrderReleasesSlotOnceDrained ==
    \A c \in COIDs :
        (state[c] \in ExchangeFinal /\ ~InEventSeq(c)) => c \notin slots

(* Never exceed the registry's capacity. *)
SlotsWithinCapacity == Cardinality(slots) =< MaxSlots

(* A live order is never queryable without being tracked, so it can never  *)
(* silently regress. Guards against a future edit making Query* reachable  *)
(* for untracked orders.                                                   *)
NoUntrackedProgress == \A c \in COIDs : (state[c] \in Live) => (c \notin slots \/ TRUE)

(***************************************************************************)
(* LIVENESS                                                                *)
(*                                                                         *)
(* Every submitted order eventually stops holding a slot, EXCEPT one that  *)
(* escalated to the operator (deliberately retained, see above). Fairness  *)
(* is weak and per-action: the reconcile thread and the hot thread both    *)
(* keep running, and the exchange eventually answers. There is no fairness *)
(* assumption on QueryLiveUnchanged -- an exchange that answers "still     *)
(* resting" forever is a legitimate behaviour, and asserting progress      *)
(* despite it is what makes this property non-trivial.                     *)
(***************************************************************************)
Fairness ==
    /\ WF_vars(Track)
    /\ WF_vars(Drain)
    /\ \A c \in COIDs : WF_vars(\E t \in ExchangeFinal : QueryAmbiguousResolves(c, t))
    /\ \A c \in COIDs : WF_vars(\E t \in ExchangeFinal : QueryLiveResolves(c, t))
    /\ \A c \in COIDs : WF_vars(Escalate(c))

SpecLive == Init /\ [][Next]_vars /\ Fairness

EventualSlotRelease ==
    \A c \in COIDs :
        [](state[c] \in Live \cup {"Ambiguous"} => <>(c \notin slots \/ state[c] = "EscalatedToOperator"))

(***************************************************************************)
(* State constraint for the safety configs. Not used by the liveness       *)
(* config -- see durable_log_recovery_liveness.cfg's own comment for why a *)
(* CONSTRAINT makes TLC's liveness checker unsound. Finiteness there comes *)
(* from the action guards instead: attempts is capped at MaxQueryAttempts, *)
(* every COID reaches a terminal or escalated state, and both queues are   *)
(* drained by fair actions.                                                *)
(***************************************************************************)
StateConstraint == Len(inbound) =< 3 /\ Len(outbound) =< 3

=============================================================================
