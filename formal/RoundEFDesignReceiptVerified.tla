-------------------- MODULE RoundEFDesignReceiptVerified --------------------
(***************************************************************************)
(* RESEARCH FORK of RoundEFDesign.tla -- NOT a patch of that file.         *)
(* Placement of this sibling (replace / INSTANCE / coordinator path) is    *)
(* deliberately unset until a design freeze; do not treat this as the      *)
(* successor of RoundEFDesign.tla.                                         *)
(*                                                                         *)
(* Same independence / no-production-traceability caveats as the parent:   *)
(* the `.x1` / `.xgc` RULES are real, tested pure functions in             *)
(* compaction_intent_codec.hpp, but no production write path raises phase, *)
(* publishes a `.x1`, or creates a `.xgc`. This fork adds one research     *)
(* variable and does not change that fact. No C++ is in scope. Do not add  *)
(* a "must fail" regression config -- there is still no historical         *)
(* incident to regress against.                                            *)
(*                                                                         *)
(* WHAT THIS FORK ADDS. docs/SPEC_INVARIANTS.md's Round E boundary: a      *)
(* future raise_intent_phase() must require the caller to hold an          *)
(* unforgeable capture receipt produced by a real codec (SealIdWatermark   *)
(* for Building->Reserved, durable Started for Reserved->StartedPublished, *)
(* C/A Authorized + proof for a terminal raise). The manager re-verifies   *)
(* receipt + Intent + current file state and MUST NOT accept a bare        *)
(* to_phase / id parameter. That receipt is NOT the `.x1` itself --        *)
(* Transition still models publishing the no-replace `.x1` path-proof.     *)
(* receiptVerified is the *precondition* authorization for the NEXT hop.   *)
(*                                                                         *)
(* OPEN (left for the freeze, not silently decided as production law):     *)
(*   * consume-per-hop (this file) vs a monotonic once-verified flag;      *)
(*   * untyped VerifyReceipt vs a receipt typed by the pending edge.       *)
(* Consume-per-hop is the stricter reading of "manager 重新验证" on every  *)
(* raise; a monotonic flag would let hops 2 and 3 ride the first verify.   *)
(***************************************************************************)
EXTENDS Naturals

Phases == {"Building", "Reserved", "StartedPublished", "PostSealFinalizing", "AbandonFinalizing"}
Terminal == {"PostSealFinalizing", "AbandonFinalizing"}

(* Legal single-hop edges, transcribed from walk_x1_chain_raw's own         *)
(* legal_edge disjunction (compaction_intent_codec.hpp) -- seq 1, 2, 3      *)
(* respectively. Unchanged from RoundEFDesign.tla. *)
LegalEdge(from, to, seq) ==
    \/ (from = "Building" /\ to = "Reserved" /\ seq = 1)
    \/ (from = "Reserved" /\ to = "StartedPublished" /\ seq = 2)
    \/ (from = "StartedPublished" /\ to \in Terminal /\ seq = 3)

VARIABLES
    phase,             \* current Intent phase
    seq,               \* next expected transition_seq (1..4; 4 = chain exhausted)
    idsBound,          \* BOOLEAN -- candidate_id/request_id bound (Reserved onward)
    gcAuthorized,      \* BOOLEAN -- a `.xgc` has been authorized for this candidate
    receiptVerified    \* BOOLEAN -- manager has re-verified the capture receipt
                       \*            that authorizes the NEXT Transition hop

vars == <<phase, seq, idsBound, gcAuthorized, receiptVerified>>

TypeOK ==
    /\ phase \in Phases
    /\ seq \in 1..4
    /\ idsBound \in BOOLEAN
    /\ gcAuthorized \in BOOLEAN
    /\ receiptVerified \in BOOLEAN

Init ==
    /\ phase = "Building"
    /\ seq = 1
    /\ idsBound = FALSE        \* Building requires ids 0/0 -- is_legal_candidate_ids_for_phase
    /\ gcAuthorized = FALSE
    /\ receiptVerified = FALSE \* genesis CREATE_NEW is not a raise; first hop needs a verify

(***************************************************************************)
(* Manager re-verifies the unforgeable capture receipt for the pending     *)
(* hop. Idempotent re-verify of an already-TRUE flag is a stutter and is   *)
(* not enabled here -- one successful verify authorizes exactly one        *)
(* subsequent Transition. Not enabled in a terminal phase: the chain is    *)
(* exhausted, so there is no next hop to authorize.                        *)
(***************************************************************************)
VerifyReceipt ==
    /\ phase \notin Terminal
    /\ ~receiptVerified
    /\ receiptVerified' = TRUE
    /\ UNCHANGED <<phase, seq, idsBound, gcAuthorized>>

(***************************************************************************)
(* One `.x1` transition. THE RESEARCH DELTA vs RoundEFDesign.tla:          *)
(* receiptVerified is a hard guard -- a naked to_phase is not a legal      *)
(* Next step. The flag is consumed (FALSE') so the next hop cannot ride    *)
(* this verify. idsBound / LegalEdge / seq rules are unchanged.            *)
(***************************************************************************)
Transition(to) ==
    /\ receiptVerified         \* no receiptVerified => Transition is not enabled
    /\ phase \notin Terminal
    /\ LegalEdge(phase, to, seq)
    /\ IF phase = "Building" THEN ~idsBound /\ idsBound' = TRUE
                             ELSE idsBound /\ UNCHANGED idsBound
    /\ phase' = to
    /\ seq' = seq + 1
    /\ receiptVerified' = FALSE
    /\ UNCHANGED gcAuthorized

(***************************************************************************)
(* `.xgc` authorization -- unchanged rule from RoundEFDesign.tla. The      *)
(* `.xgc` is a different receipt (GC-authorization, post-terminal) and     *)
(* does not consume or require receiptVerified.                            *)
(***************************************************************************)
AuthorizeGc ==
    /\ phase \in Terminal
    /\ gcAuthorized' = TRUE
    /\ UNCHANGED <<phase, seq, idsBound, receiptVerified>>

Next ==
    \/ VerifyReceipt
    \/ \E to \in Phases : Transition(to)
    \/ AuthorizeGc

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* SAFETY                                                                  *)
(***************************************************************************)

(* ids are bound (nonzero) from Reserved onward, and never before --       *)
(* is_legal_candidate_ids_for_phase's own rule, restated as a state         *)
(* invariant over the whole chain rather than a per-call check.            *)
IdsBoundIffPastBuilding == idsBound <=> (phase # "Building")

(* GC can only ever be authorized for a candidate whose chain actually      *)
(* reached a terminal phase -- never for one still Building/Reserved/       *)
(* StartedPublished. *)
GcOnlyAfterTerminal == gcAuthorized => phase \in Terminal

(* The chain never exceeds its 3-transition length (seq caps at 4, meaning *)
(* "exhausted" -- walk_x1_chain_raw's own TooManyFrames rejection of a      *)
(* fourth frame). *)
ChainBoundedAtThreeTransitions == seq =< 4

(* "没有 receiptVerified 就不能 Transition" -- state form. If the           *)
(* receiptVerified guard is dropped from Transition, Init enables           *)
(* Transition("Reserved") while the flag is FALSE, and TLC reports this.   *)
(* Consume-per-hop means (phase # "Building") => receiptVerified is FALSE  *)
(* after every successful raise (legal: waiting for the next hop's         *)
(* verify), so that monotonic restatement is intentionally NOT an          *)
(* invariant of this fork.                                                 *)
TransitionNeverEnabledNaked ==
    (\E to \in Phases : ENABLED Transition(to)) => receiptVerified

(* Same rule as a temporal action property: any actual phase change still  *)
(* required a live receiptVerified in the pre-state. Stuttering and        *)
(* AuthorizeGc / VerifyReceipt (phase unchanged) are unconstrained.        *)
NoTransitionWithoutReceipt == [][phase' # phase => receiptVerified]_vars

(***************************************************************************)
(* LIVENESS: a Building intent, once the receipt/transition dance starts,  *)
(* always eventually reaches a terminal phase. Weak fairness on BOTH       *)
(* VerifyReceipt and Transition -- fairness on Transition alone is         *)
(* vacuous at Init (Transition is not enabled until a verify), and the     *)
(* chain would stutter in Building forever. AuthorizeGc is still not       *)
(* assumed to ever happen.                                                 *)
(***************************************************************************)
Fairness ==
    /\ WF_vars(VerifyReceipt)
    /\ \A to \in Phases : WF_vars(Transition(to))

SpecLive == Init /\ [][Next]_vars /\ Fairness

EventuallyTerminal == <>(phase \in Terminal)

================================================================================
