----------------------------- MODULE RoundEFDesign -----------------------------
(***************************************************************************)
(* An INDEPENDENT, forward-looking model of the Intent phase / `.x1` chain /*)
(* `.xgc` GC-authorization state machine whose RULES already exist, tested, *)
(* in native/include/hengyuan/compaction_intent_codec.hpp (walk_x1_chain_   *)
(* raw, is_legal_candidate_id_transition, is_legal_cleanup_auth_flags) --   *)
(* but for which NO production write path exists anywhere in this codebase *)
(* yet. Round D deliberately never calls any of these; see compaction_     *)
(* intent_codec.hpp's own SCOPE comment.                                   *)
(*                                                                         *)
(* THIS IS NOT AN EXTENSION OF RoundDActual.tla AND HAS NO PRODUCTION      *)
(* TRACEABILITY OF ITS OWN. It formalizes rules that are real, tested pure *)
(* functions today, so that a future Round E/F implementation has a        *)
(* checked reference for the state machine shape before writing the real   *)
(* writer/receipt code -- the same "grounding must exist before modeling"  *)
(* discipline formal/README.md's own Scope section states for              *)
(* durable_log_recovery.tla, applied in the other direction: here the      *)
(* CODE exists (and is unit-tested) before any REAL durable write path     *)
(* does, so this model has code-level grounding but explicitly no          *)
(* production-behavior grounding -- there is no real bug this model could  *)
(* be shown to catch, unlike RoundDActual.tla's three regression controls. *)
(* Do not add a "must fail" regression config here for that reason: there  *)
(* is no historical incident to regress against yet.                       *)
(***************************************************************************)
EXTENDS Naturals

Phases == {"Building", "Reserved", "StartedPublished", "PostSealFinalizing", "AbandonFinalizing"}
Terminal == {"PostSealFinalizing", "AbandonFinalizing"}

(* Legal single-hop edges, transcribed from walk_x1_chain_raw's own         *)
(* legal_edge disjunction (compaction_intent_codec.hpp) -- seq 1, 2, 3      *)
(* respectively. *)
LegalEdge(from, to, seq) ==
    \/ (from = "Building" /\ to = "Reserved" /\ seq = 1)
    \/ (from = "Reserved" /\ to = "StartedPublished" /\ seq = 2)
    \/ (from = "StartedPublished" /\ to \in Terminal /\ seq = 3)

VARIABLES
    phase,          \* current Intent phase
    seq,            \* next expected transition_seq (1..4; 4 = chain exhausted)
    idsBound,       \* BOOLEAN -- candidate_id/request_id bound (Reserved onward)
    gcAuthorized    \* BOOLEAN -- a `.xgc` has been authorized for this candidate

vars == <<phase, seq, idsBound, gcAuthorized>>

TypeOK ==
    /\ phase \in Phases
    /\ seq \in 1..4
    /\ idsBound \in BOOLEAN
    /\ gcAuthorized \in BOOLEAN

Init ==
    /\ phase = "Building"
    /\ seq = 1
    /\ idsBound = FALSE        \* Building requires ids 0/0 -- is_legal_candidate_ids_for_phase
    /\ gcAuthorized = FALSE

(***************************************************************************)
(* One `.x1` transition. idsBound flips FALSE->TRUE exactly once, at the   *)
(* Building->Reserved edge (seq=1) -- is_legal_candidate_id_transition's    *)
(* one-time 0/0->nonzero/nonzero step. Every other edge requires ids       *)
(* already bound and leaves them unchanged (immutable once bound).         *)
(***************************************************************************)
Transition(to) ==
    /\ phase \notin Terminal
    /\ LegalEdge(phase, to, seq)
    /\ IF phase = "Building" THEN ~idsBound /\ idsBound' = TRUE
                             ELSE idsBound /\ UNCHANGED idsBound
    /\ phase' = to
    /\ seq' = seq + 1
    /\ UNCHANGED gcAuthorized

(***************************************************************************)
(* `.xgc` authorization -- only reachable once the phase itself is         *)
(* terminal (chain complete), matching is_legal_cleanup_auth_flags'        *)
(* disposition table, which is keyed on terminal_disposition values that   *)
(* only make sense post-chain. Authorizing twice for the same candidate is *)
(* not modeled as illegal here (a real implementation's no-replace publish *)
(* semantics would make a second attempt idempotent, same as Round D's own *)
(* genesis write -- see RoundDActual.tla) so this stays a monotonic flag.  *)
(***************************************************************************)
AuthorizeGc ==
    /\ phase \in Terminal
    /\ gcAuthorized' = TRUE
    /\ UNCHANGED <<phase, seq, idsBound>>

Next ==
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

(***************************************************************************)
(* LIVENESS: a Building intent, once transitions start, always eventually  *)
(* reaches a terminal phase (the chain does not get stuck mid-flight       *)
(* forever). Weak fairness on Transition only -- AuthorizeGc is a distinct,*)
(* separately-timed operator/system action this model does not assume ever*)
(* happens.                                                                *)
(***************************************************************************)
Fairness == \A to \in Phases : WF_vars(Transition(to))

SpecLive == Init /\ [][Next]_vars /\ Fairness

EventuallyTerminal == <>(phase \in Terminal)

=================================================================================
