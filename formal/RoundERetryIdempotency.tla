-------------------------- MODULE RoundERetryIdempotency --------------------------
(***************************************************************************)
(* Single-process retry-idempotency state machine for raise_intent_phase() *)
(* on one build_nonce. NOT a two-caller deadlock model -- same shape as    *)
(* RoundELockOrder.tla. The question is whether a same-process retry can   *)
(* let Step 1 (SealIdWatermark advance) bind two different candidate ids.  *)
(*                                                                         *)
(* Grounding (not re-derived here):                                        *)
(*   * IntentPhaseAdvancer::raise_intent_phase(build_nonce) does three     *)
(*     writes that may fail and be retried by the same caller:             *)
(*       1. SealIdWatermark advance (REPLACE / CREATE_NEW-bootstrap) --    *)
(*          produces the (candidate_id, request_id) pair this call binds.  *)
(*       2. `.jhw` CREATE_NEW -- depends on Step 1's candidate_id.         *)
(*       3. `.x1` CREATE_NEW (seq=1) -- depends on Step 1's two ids.       *)
(*     Intent REPLACE is out of scope (no idempotent-retry guard of this   *)
(*     kind).                                                              *)
(*   * WatermarkAdvanceProvenanceMemory.advanced is armed on               *)
(*     DurablyPublished (CREATE or byte-equal collide) immediately -- the  *)
(*     id is spent (L4: never reclaim), later steps succeeding or not.     *)
(*     A later call with the same build_nonce that sees                   *)
(*     `has_value() && advanced` reuses the bound pair and does not        *)
(*     re-read / re-write the watermark.                                   *)
(*   * JhwWriteProvenanceMemory / X1WriteProvenanceMemory arm only on      *)
(*     PublishedNamespaceUncertain. Determinate success or failure of      *)
(*     those CREATE_NEW steps can be re-derived; they are not this         *)
(*     control's target.                                                   *)
(*                                                                         *)
(* THIS MODEL HAS A NEGATIVE CONTROL, same inversion discipline as         *)
(* RoundELockOrder.tla -- NOT the no-control precedent of                  *)
(* RoundEFDesignReceiptVerified.tla. _bug.cfg MUST print                   *)
(* `Invariant NeverRebindsCandidateId is violated` so it matches this      *)
(* repo's existing CI grep (`grep -q "Invariant $inv is violated"`).       *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS
    MaxCandidateId,
    \* Finite id space. 2 is enough: the bug must be able to pick a
    \* *different* id on a second AdvanceWatermark, or the control is vacuous.
    ReuseBoundIdsOnRetry
    \* MODEL SWITCH.
    \*   TRUE  = correct Next: AdvanceWatermark requires ~watermarkAdvanced
    \*           (reuse WatermarkAdvanceProvenanceMemory; do not re-derive)
    \*   FALSE = the Next with that guard dropped -- MUST violate
    \*           NeverRebindsCandidateId (RoundERetryIdempotency_bug.cfg)

ASSUME MaxCandidateId \in Nat /\ MaxCandidateId >= 2

CandidateIds == 1..MaxCandidateId
Unbound      == 0

VARIABLES
    watermarkAdvanced,      \* BOOLEAN -- Step 1 DurablyPublished for this nonce
    boundCandidateId,       \* Unbound, or the id the process would use *now*
    firstBoundCandidateId,  \* ghost: first DurablyPublished bind; sticky
    jhwWritten,             \* BOOLEAN -- Step 2 `.jhw` CREATE_NEW landed
    x1Written               \* BOOLEAN -- Step 3 `.x1` seq=1 CREATE_NEW landed

vars == <<watermarkAdvanced, boundCandidateId, firstBoundCandidateId,
           jhwWritten, x1Written>>

TypeOK ==
    /\ watermarkAdvanced \in BOOLEAN
    /\ boundCandidateId \in {Unbound} \cup CandidateIds
    /\ firstBoundCandidateId \in {Unbound} \cup CandidateIds
    /\ jhwWritten \in BOOLEAN
    /\ x1Written \in BOOLEAN

Init ==
    /\ watermarkAdvanced = FALSE
    /\ boundCandidateId = Unbound
    /\ firstBoundCandidateId = Unbound
    /\ jhwWritten = FALSE
    /\ x1Written = FALSE

(***************************************************************************)
(* Step 1, DurablyPublished. The pair (candidate_id, request_id) is spent  *)
(* as one atomic bind -- request_id is not a separate variable (see the    *)
(* module-end note). Correct Next is enabled only while ~watermarkAdvanced *)
(* -- a retry after advanced reuses firstBoundCandidateId and never comes  *)
(* through this action. _bug.cfg drops that conjunct by setting            *)
(* ReuseBoundIdsOnRetry = FALSE, so a second fire may pick a new id.       *)
(***************************************************************************)
AdvanceWatermark(id) ==
    /\ id \in CandidateIds
    /\ (ReuseBoundIdsOnRetry => ~watermarkAdvanced)
    /\ watermarkAdvanced' = TRUE
    /\ boundCandidateId' = id
    /\ firstBoundCandidateId' = IF firstBoundCandidateId = Unbound
                                THEN id
                                ELSE firstBoundCandidateId
    /\ UNCHANGED <<jhwWritten, x1Written>>

(***************************************************************************)
(* Steps 2 and 3, option (a): no provenance-memory / Uncertain state.      *)
(* Both require Step 1 already DurablyPublished. Determinate CREATE_NEW    *)
(* success/failure would be re-derivable; that claim is why they are not   *)
(* this control's target.                                                  *)
(***************************************************************************)
WriteJhw ==
    /\ watermarkAdvanced
    /\ ~jhwWritten
    /\ jhwWritten' = TRUE
    /\ UNCHANGED <<watermarkAdvanced, boundCandidateId, firstBoundCandidateId,
                    x1Written>>

WriteX1 ==
    /\ watermarkAdvanced
    /\ ~x1Written
    /\ x1Written' = TRUE
    /\ UNCHANGED <<watermarkAdvanced, boundCandidateId, firstBoundCandidateId,
                    jhwWritten>>

Next ==
    \/ \E id \in CandidateIds : AdvanceWatermark(id)
    \/ WriteJhw
    \/ WriteX1

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* SAFETY                                                                  *)
(*                                                                         *)
(* NeverRebindsCandidateId: once Step 1 has DurablyPublished, the live     *)
(* bind equals the first bind. firstBoundCandidateId is the history        *)
(* variable -- a state invariant cannot see "the previous state's id"      *)
(* without it, and CI greps INVARIANT violations, not temporal properties. *)
(* The correct ~watermarkAdvanced guard is what maintains this: the action *)
(* that would overwrite boundCandidateId is disabled after the first       *)
(* spend. Dropping the guard makes AdvanceWatermark(otherId) reachable.    *)
(***************************************************************************)
NeverRebindsCandidateId ==
    watermarkAdvanced => boundCandidateId = firstBoundCandidateId

(* Live bind is present iff Step 1 has published -- both sides of the      *)
(* first AdvanceWatermark, and never cleared. *)
IdsBoundIffAdvanced ==
    watermarkAdvanced <=> (boundCandidateId # Unbound)

================================================================================
