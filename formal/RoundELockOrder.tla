----------------------------- MODULE RoundELockOrder -----------------------------
(***************************************************************************)
(* Single-process lock-order state machine for Round E's two-lease         *)
(* protocol. NOT a two-caller deadlock model -- there is one actor, and    *)
(* the question is whether that actor can ever hold SealJournalStoreLease  *)
(* without CandidateLease already being held.                              *)
(*                                                                         *)
(* Grounding (not re-derived here):                                        *)
(*   * seal_journal_store_lease.hpp LOCK ORDER: "CandidateLease MUST be    *)
(*     acquired before SealJournalStoreLease, and released after it        *)
(*     (SealJournalStoreLease released first). ... This order is           *)
(*     documentation-enforced only -- there is no compiler mechanism       *)
(*     preventing a future caller from acquiring them in the opposite      *)
(*     order".                                                             *)
(*   * intent_phase_advancer.hpp: constructor takes two already-held       *)
(*     references; the class itself does not acquire or release either     *)
(*     lease. The rule binds whoever constructs an IntentPhaseAdvancer.    *)
(*   * `.jhw` CREATE_NEW is raise_intent_phase() Step 1b -- after          *)
(*     SealIdWatermark advance, before `.x1` -- and requires               *)
(*     SealJournalStoreLease already held for the whole call.              *)
(*                                                                         *)
(* THIS MODEL HAS A NEGATIVE CONTROL. RoundEFDesignReceiptVerified.tla     *)
(* (a prior research fork) deliberately had none -- there was no           *)
(* historical incident to regress against. This is different: the lock     *)
(* order is a real, documentation-only rule with a named inverted          *)
(* config. _bug.cfg MUST print `Invariant LockOrderRespected is            *)
(* violated` so it matches this repo's existing CI grep                    *)
(* (`grep -q "Invariant $inv is violated"`). Do not copy the               *)
(* no-negative-control precedent.                                          *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS
    RequireCandidateHeldForSealJournal
    \* MODEL SWITCH.
    \*   TRUE  = correct Next: AcquireSealJournalLease requires candidateLeaseHeld
    \*   FALSE = the Next with that guard dropped -- MUST violate
    \*           LockOrderRespected (RoundELockOrder_bug.cfg)

VARIABLES
    candidateLeaseHeld,     \* BOOLEAN -- CandidateLease currently held
    sealJournalLeaseHeld,   \* BOOLEAN -- SealJournalStoreLease currently held
    jhwWritten              \* BOOLEAN -- Step 1b `.jhw` CREATE_NEW has landed

vars == <<candidateLeaseHeld, sealJournalLeaseHeld, jhwWritten>>

TypeOK ==
    /\ candidateLeaseHeld \in BOOLEAN
    /\ sealJournalLeaseHeld \in BOOLEAN
    /\ jhwWritten \in BOOLEAN

Init ==
    /\ candidateLeaseHeld = FALSE
    /\ sealJournalLeaseHeld = FALSE
    /\ jhwWritten = FALSE

(***************************************************************************)
(* Outer lock. No compiler mechanism enforces that this happens first --   *)
(* the documentation-only rule is encoded as the guard on the inner        *)
(* acquire below, and as LockOrderRespected.                               *)
(***************************************************************************)
AcquireCandidateLease ==
    /\ ~candidateLeaseHeld
    /\ candidateLeaseHeld' = TRUE
    /\ UNCHANGED <<sealJournalLeaseHeld, jhwWritten>>

(***************************************************************************)
(* Inner lock. Correct Next requires candidateLeaseHeld already TRUE --    *)
(* "must have been held before this acquire", the history reading of the   *)
(* lock-order sentence, stated as a guard. _bug.cfg drops that conjunct    *)
(* by setting RequireCandidateHeldForSealJournal = FALSE.                  *)
(***************************************************************************)
AcquireSealJournalLease ==
    /\ ~sealJournalLeaseHeld
    /\ (RequireCandidateHeldForSealJournal => candidateLeaseHeld)
    /\ sealJournalLeaseHeld' = TRUE
    /\ UNCHANGED <<candidateLeaseHeld, jhwWritten>>

(***************************************************************************)
(* raise_intent_phase() Step 1b: CREATE_NEW `.jhw`. The advancer does not  *)
(* take either lease; both must already be held when this step runs.       *)
(* CREATE_NEW is once -- a landed `.jhw` stays written across later        *)
(* release/re-acquire of the leases.                                       *)
(***************************************************************************)
WriteJhw ==
    /\ candidateLeaseHeld
    /\ sealJournalLeaseHeld
    /\ ~jhwWritten
    /\ jhwWritten' = TRUE
    /\ UNCHANGED <<candidateLeaseHeld, sealJournalLeaseHeld>>

ReleaseSealJournalLease ==
    /\ sealJournalLeaseHeld
    /\ sealJournalLeaseHeld' = FALSE
    /\ UNCHANGED <<candidateLeaseHeld, jhwWritten>>

(***************************************************************************)
(* Outer release. Inner lock must already be gone -- released first.       *)
(***************************************************************************)
ReleaseCandidateLease ==
    /\ candidateLeaseHeld
    /\ ~sealJournalLeaseHeld
    /\ candidateLeaseHeld' = FALSE
    /\ UNCHANGED <<sealJournalLeaseHeld, jhwWritten>>

Next ==
    \/ AcquireCandidateLease
    \/ AcquireSealJournalLease
    \/ WriteJhw
    \/ ReleaseSealJournalLease
    \/ ReleaseCandidateLease

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* SAFETY                                                                  *)
(*                                                                         *)
(* Guard-implies-invariant: the correct AcquireSealJournalLease guard is   *)
(* what maintains this. When sealJournalLeaseHeld is TRUE,                 *)
(* candidateLeaseHeld is already TRUE (it was required to become TRUE      *)
(* first, and ReleaseCandidateLease refuses to drop it while the inner     *)
(* lock is still held). Dropping the acquire guard makes                   *)
(* sealJournalLeaseHeld /\ ~candidateLeaseHeld reachable, which is the     *)
(* inverted-order state the documentation-only rule exists to forbid.      *)
(***************************************************************************)
LockOrderRespected == sealJournalLeaseHeld => candidateLeaseHeld

================================================================================
