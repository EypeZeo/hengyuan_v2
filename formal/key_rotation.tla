-------------------------- MODULE key_rotation --------------------------
(***************************************************************************)
(* KEY-ROTATE-008: two-step key rotation, crash-safety model.              *)
(*                                                                         *)
(* WHY THIS MODEL EXISTS. formal/README.md's "Still not modeled" section  *)
(* named two-step key rotation as the highest-value unmodeled crash        *)
(* window: rotate_active_key() is a two-step DURABLE protocol, and a       *)
(* crash between the two steps leaves on-disk state that a restart has to  *)
(* interpret. Unit tests reach that window only by construction (undoing   *)
(* step 2 byte-for-byte); a model checks every crash cut exhaustively.     *)
(*                                                                         *)
(* THE REAL PROTOCOL (native/include/hengyuan/durable_audit_sink.hpp,      *)
(* rotate_active_key(), lines 502-551):                                    *)
(*                                                                         *)
(*   Step 1: durably append a KeyRotated frame to the SIDECAR log (signed  *)
(*           under the NEW key, old key carried as payload), then durably  *)
(*           write the sidecar's own tip anchor. Two separate fsyncs --    *)
(*           either can be the only thing a crash lets land.               *)
(*   Step 2: re-anchor the MAIN log's tip under the NEW key. LEGALLY       *)
(*           SKIPPED when the main log is still empty (nothing to anchor   *)
(*           yet -- the first append_durable() writes the first frame +    *)
(*           anchor under the new key).                                    *)
(*   Only after both steps are durably done does active_key_id_ itself     *)
(*   flip. Any failure along the way leaves it untouched.                  *)
(*                                                                         *)
(*   Recovery (finalize_scan_with_anchor_check(), lines 790-862): the      *)
(*   main tip anchor's key_id must equal the configured active key. A      *)
(*   mismatch is accepted ONLY when all three of these hold -- and only    *)
(*   then is step 2 completed (interrupted_rotation_, lines 746-754):      *)
(*     (a) the restart is configured with the NEW key,                     *)
(*     (b) the sidecar's last KeyRotated record proves old -> new          *)
(*         (last_rotation_ is set by the sidecar FRAME scan, line 934 --   *)
(*         note: the sidecar's own tip anchor does NOT gate this: a torn   *)
(*         sidecar anchor only sets rotation_fenced_, which gates future   *)
(*         rotations, not the repair decision),                            *)
(*     (c) the main anchor is still signed by the OLD key.                 *)
(*   Any other mismatch (no rotation record, wrong payload, config that    *)
(*   matches neither, unreadable anchor) is Corrupt: the sink fences and   *)
(*   nothing is auto-repaired.                                             *)
(*                                                                         *)
(*   Keys and retirement (key_ring.hpp, audit KEY-RETIRE-009): a key that  *)
(*   ever signed a retained frame must still be loadable at recovery;      *)
(*   retiring it turns the whole log Corrupt and fences the sink.          *)
(*   "the active key flipped from old to new" is a DIFFERENT fact from     *)
(*   "the old key was retired from the ring" -- modeled as separate        *)
(*   variables (active_key vs ring_keys) and tied by                       *)
(*   NoDataUnverifiableAfterRotation below.                                *)
(*                                                                         *)
(* MODELING CONVENTIONS                                                    *)
(*                                                                         *)
(*   Volatile (wiped by Crash): phase, active_key, sink_fenced,            *)
(*       rotation_fenced, io_failure_injected.                             *)
(*   Durable (survives Crash): main_log_empty, main_frames_signed_by,      *)
(*       main_anchor_signed_by, sidecar_frame_durable,                     *)
(*       sidecar_anchor_durable, last_rotation.                            *)
(*   Boot choices (operator facts, re-picked at every RecoveryScan):       *)
(*       configured_active_key_id, ring_keys.                              *)
(*   Ghost: repaired_without_proof (true iff recovery ever rewrote the     *)
(*       main anchor without the (a)(b)(c) proof).                         *)
(*                                                                         *)
(*   Crash cuts the model explores exhaustively (the Crash action fires    *)
(*   from every Running state):                                            *)
(*     1. crash before step 1         -- nothing durable, no repair path   *)
(*     2. crash between step 1a and    -- sidecar frame durable, sidecar   *)
(*        1b (frame vs sidecar anchor)    anchor not                        *)
(*     3. crash between step 1 and 2   -- sidecar complete, main anchor    *)
(*         (canonical interrupted         still old                         *)
(*         rotation)                                                       *)
(*     4. crash inside step 2's anchor -- main anchor left unreadable      *)
(*         write (torn)                  ("none"), non-empty log            *)
(*     5. crash after step 2           -- rotation complete                *)
(*     6. crash around an empty-log    -- step 2 legally skipped; recovery *)
(*         rotation                       is Clean, never a repair         *)
(*   Combined with the boot's nondeterministic choice of configured key    *)
(*   and ring contents, every (config, durable-state) mismatch combo the   *)
(*   review checklist demands is reached.                                  *)
(*                                                                         *)
(* REGRESSION CONTROL. RepairOnlyWhenProven=FALSE reinstates the simpler   *)
(* (wrong) protocol: recovery looks only at the main anchor's key_id vs    *)
(* the configured key and re-anchors on ANY mismatch, ignoring the         *)
(* sidecar evidence. It MUST violate RepairOnlyOnProvenMismatch.           *)
(*                                                                         *)
(* LIVENESS. A crash between the two steps does not GUARANTEE recovery by  *)
(* itself: Crash can recur and the scheduler can refuse to run             *)
(* RecoveryScan. The property is therefore checked only under (1) a        *)
(* bounded crash budget (crashes_left, an action guard -- the same reason  *)
(* durable_log_recovery_liveness.cfg must not use a CONSTRAINT: a pruned   *)
(* successor set makes TLC treat stuttering as a legal infinite behavior   *)
(* and report a spurious violation) and (2) WF_vars(RecoveryScan): the     *)
(* process is eventually restarted and its constructor's scan (including   *)
(* the interrupted-rotation re-anchor I/O) eventually succeeds. That       *)
(* fairness assumption matches the real operation: a crashed process is    *)
(* restarted by the supervisor, and DurableAuditSink's constructor         *)
(* unconditionally runs run_rotation_recovery_scan() + run_recovery_scan() *)
(* (durable_audit_sink.hpp:320-331), completing an interrupted rotation's  *)
(* step 2 inside that same constructor (lines 746-754).                    *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS
    MaxCrashes,          \* modeling budget, NOT a design bound
    RepairOnlyWhenProven \* MODEL SWITCH: TRUE = current design (sidecar
                         \* proof required); FALSE = one-step bug (repair on
                         \* any anchor key mismatch vs the configured key)

ASSUME MaxCrashes \in Nat
ASSUME RepairOnlyWhenProven \in BOOLEAN

\* Exactly one rotation (old -> new) is modeled. A chain of rotations has
\* the same two-step structure per link, and last_rotation is the sidecar's
\* LAST claim -- the only one whose step 2 could still be outstanding (the
\* real code's own argument, durable_audit_sink.hpp:929-933).
KeyIds == {"old", "new"}

VARIABLES
    phase,                     \* {"Running", "Crashed"}
    active_key,                \* in-memory signing key; flips ONLY after both
                               \* steps are durable (durable-before-flip)
    configured_active_key_id,  \* the key the restart was configured with --
                               \* a SEPARATE fact from what the sidecar and the
                               \* main log each actually record
    ring_keys,                 \* keys currently loaded in the KeyRing (audit
                               \* KEY-RETIRE-009: recovery requires every key
                               \* the log still references to be loadable)
    main_log_empty,            \* durable: main log has no frames yet
    main_frames_signed_by,     \* durable: key that signed the main log's
                               \* frames ("none" while the log is empty)
    main_anchor_signed_by,     \* durable: key signing the main tip anchor
                               \* ("none" = no readable anchor on disk)
    sidecar_frame_durable,     \* durable: KeyRotated frame fully written +
                               \* fsynced. Deliberately SEPARATE from ...
    sidecar_anchor_durable,    \* durable: the sidecar's own tip anchor --
                               \* two independent persistence facts, each can
                               \* be the only one a crash lets land
    last_rotation,             \* durable: sidecar's last rotation claim.
                               \* SUBSET {<<"old","new">>}: the empty set is
                               \* "no rotation yet", the singleton is the
                               \* old->new claim. (A string sentinel would
                               \* trip TLC's fingerprinting against the
                               \* tuple -- the IdleWriter lesson from
                               \* durable_log_recovery.tla, README trap #7.)
    sink_fenced,               \* volatile: main-log fence (fail-closed)
    rotation_fenced,           \* volatile: rotation fence; gates ONLY
                               \* rotate_active_key(), never append_durable()
    io_failure_injected,       \* volatile: some rotation step's I/O returned
                               \* failure (NOT a crash) -- sticky per process
    crashes_left,              \* modeling budget (action guard, not CONSTRAINT)
    repaired_without_proof,    \* ghost: recovery rewrote the main anchor
                               \* without the (a)(b)(c) proof
    flipped                    \* ghost: rotate_active_key() actually performed
                               \* the durable-before-flip flip (the constructor
                               \* starting with the CONFIGURED key is not one)

vars == <<phase, active_key, configured_active_key_id, ring_keys, main_log_empty,
          main_frames_signed_by, main_anchor_signed_by, sidecar_frame_durable,
          sidecar_anchor_durable, last_rotation, sink_fenced, rotation_fenced,
          io_failure_injected, crashes_left, repaired_without_proof, flipped>>

TypeOK ==
    /\ phase \in {"Running", "Crashed"}
    /\ active_key \in KeyIds
    /\ configured_active_key_id \in KeyIds
    /\ ring_keys \subseteq KeyIds
    /\ main_log_empty \in BOOLEAN
    /\ main_frames_signed_by \in KeyIds \cup {"none"}
    /\ main_anchor_signed_by \in KeyIds \cup {"none"}
    /\ sidecar_frame_durable \in BOOLEAN
    /\ sidecar_anchor_durable \in BOOLEAN
    /\ last_rotation \subseteq {<<"old", "new">>}
    /\ sink_fenced \in BOOLEAN
    /\ rotation_fenced \in BOOLEAN
    /\ io_failure_injected \in BOOLEAN
    /\ crashes_left \in 0..MaxCrashes
    /\ repaired_without_proof \in BOOLEAN
    /\ flipped \in BOOLEAN

\* Well-formedness the real protocol guarantees: an anchor only ever exists
\* behind a non-empty log (the first append writes frame and anchor together;
\* an empty log has no anchor on disk).
AnchorImpliesNonEmptyLog ==
    main_anchor_signed_by \in KeyIds => ~main_log_empty

Init ==
    \* A brand-new store: the first action is the first constructor's scan
    \* (the constructor always runs the recovery scans, even on an empty log).
    /\ phase = "Crashed"
    /\ active_key = "old"
    /\ configured_active_key_id = "old"
    /\ ring_keys = {"old", "new"}
    /\ main_log_empty = TRUE
    /\ main_frames_signed_by = "none"
    /\ main_anchor_signed_by = "none"
    /\ sidecar_frame_durable = FALSE
    /\ sidecar_anchor_durable = FALSE
    /\ last_rotation = {}
    /\ sink_fenced = FALSE
    /\ rotation_fenced = FALSE
    /\ io_failure_injected = FALSE
    /\ crashes_left = MaxCrashes
    /\ repaired_without_proof = FALSE
    /\ flipped = FALSE

\* Every key the MAIN log's retained content still references: its frames and
\* its tip anchor. (The SIDECAR frame's key is deliberately NOT here: an
\* unloadable sidecar key fails the sidecar scan and sets rotation_fenced_
\* only -- it never fences the main log, durable_audit_sink.hpp:914-916.)
MainLogReferencedKeys ==
    IF main_log_empty THEN {} ELSE {main_frames_signed_by, main_anchor_signed_by}

(***************************************************************************)
(* Constructor scan + recovery (run_rotation_recovery_scan() then          *)
(* run_recovery_scan() -> finalize_scan_with_anchor_check(), and the       *)
(* interrupted-rotation completion at lines 746-754).                      *)
(***************************************************************************)
RecoveryScan ==
    /\ phase = "Crashed"
    /\ phase' = "Running"
    \* A new process generation starts here: `flipped` (whether THIS generation
    \* performed the flip) resets -- a previous generation's flip does not
    \* constrain this one (an operator may legitimately reconfigure back to
    \* the old key after a completed rotation and append under it).
    /\ flipped' = FALSE
    \* Boot facts: the operator's config and key loading are re-picked every
    \* restart (a restart may configure either key; the ring may lack a key
    \* the log still references -- KEY-RETIRE-009's fail-closed outcome).
    /\ configured_active_key_id' \in KeyIds
    /\ active_key' = configured_active_key_id'
    /\ ring_keys' \in SUBSET KeyIds
    \* Sidecar scan verdict (run_rotation_recovery_scan() returning false):
    \* the sidecar is inconsistent iff its frame and its own anchor disagree
    \* (frame without anchor, or anchor without frame, is a torn sidecar),
    \* OR the KeyRotated frame's key (always the new key) is not loadable
    \* (durable_audit_sink.hpp:914-916). This gates ONLY future rotations.
    /\ rotation_fenced' = ((sidecar_frame_durable # sidecar_anchor_durable)
                           \/ (sidecar_frame_durable /\ ~("new" \in ring_keys')))
    /\ LET cfg == configured_active_key_id' IN
       IF main_log_empty THEN
           \* Empty main log: there is nothing for the anchor check to
           \* mismatch. This is the LEGAL skip of step 2 (a rotation on an
           \* empty log never had a main anchor to re-anchor) -- Clean, and
           \* crucially NOT the interrupted-rotation repair branch.
           /\ UNCHANGED <<main_anchor_signed_by, sink_fenced, repaired_without_proof>>
       ELSE IF main_anchor_signed_by = "none" THEN
           \* Non-empty log with no readable anchor (missing or torn):
           \* IoError/Corrupt (lines 796-801, 808-809) -- fail closed.
           /\ sink_fenced' = TRUE
           /\ UNCHANGED <<main_anchor_signed_by, repaired_without_proof>>
       ELSE IF ~(MainLogReferencedKeys \subseteq ring_keys') THEN
           \* A key the log still references is not loadable (retired or
           \* never loaded): the scan cannot verify that content (lines
           \* 670, 812, 748) -- Corrupt -- fail closed.
           /\ sink_fenced' = TRUE
           /\ UNCHANGED <<main_anchor_signed_by, repaired_without_proof>>
       ELSE IF main_anchor_signed_by = cfg THEN
           \* Anchor key matches the configured key: Clean/Recovered, no
           \* repair, no fence.
           /\ UNCHANGED <<main_anchor_signed_by, sink_fenced, repaired_without_proof>>
       ELSE
           \* MISMATCH: the anchor is signed by a key that is not the one
           \* this restart was configured with.
           /\ IF RepairOnlyWhenProven THEN
                  IF cfg = "new" /\ last_rotation = {<<"old","new">>}
                                 /\ main_anchor_signed_by = "old"
                                 /\ "new" \in ring_keys' THEN
                      \* (a)(b)(c) all hold -- plus the re-anchor's own
                      \* precondition that the configured key is loadable
                      \* (line 748: key_ring_.active_key(active_key_id_)
                      \* failing would return Corrupt here): an interrupted
                      \* rotation that lost nothing. Complete step 2:
                      \* re-anchor under the new key (lines 843-847 +
                      \* 746-754). Repair, no fence.
                      /\ main_anchor_signed_by' = "new"
                      /\ UNCHANGED <<sink_fenced, repaired_without_proof>>
                  ELSE
                      \* Mismatch the sidecar cannot vouch for (no rotation
                      \* record, wrong payload, or a config matching neither
                      \* key): Corrupt (line 843-845) -- fail closed, never
                      \* auto-repaired.
                      /\ sink_fenced' = TRUE
                      /\ UNCHANGED <<main_anchor_signed_by, repaired_without_proof>>
              ELSE
                  \* ONE-STEP BUG (regression config only): decide from the
                  \* anchor key alone, ignore the sidecar -- repair on ANY
                  \* mismatch. The ghost records every such repair whose
                  \* (a)(b)(c) proof did not actually hold.
                  /\ main_anchor_signed_by' = cfg
                  /\ repaired_without_proof' =
                         (repaired_without_proof
                          \/ ~(cfg = "new" /\ last_rotation = {<<"old","new">>}
                                        /\ main_anchor_signed_by = "old"))
                  /\ UNCHANGED sink_fenced
    /\ UNCHANGED <<main_log_empty, main_frames_signed_by, sidecar_frame_durable,
                   sidecar_anchor_durable, last_rotation, io_failure_injected,
                   crashes_left>>

(***************************************************************************)
(* Crash: volatile state dies, durable state and ghosts survive.           *)
(***************************************************************************)
Crash ==
    /\ phase = "Running"
    /\ crashes_left > 0
    /\ crashes_left' = crashes_left - 1
    /\ phase' = "Crashed"
    /\ sink_fenced' = FALSE
    /\ rotation_fenced' = FALSE
    /\ io_failure_injected' = FALSE
    \* active_key / configured / ring_keys survive as stale until the next
    \* RecoveryScan overwrites them; durable facts and ghosts are untouched.
    /\ UNCHANGED <<active_key, configured_active_key_id, ring_keys, main_log_empty,
                   main_frames_signed_by, main_anchor_signed_by, sidecar_frame_durable,
                   sidecar_anchor_durable, last_rotation, repaired_without_proof,
                   flipped>>

(***************************************************************************)
(* First append_durable(): makes the main log non-empty and writes the     *)
(* first frame + tip anchor under the current active key. Modeled as one   *)
(* atomic step: the append's own late-fsync window (frame lands, anchor     *)
(* lags) is durable_log_recovery.tla's subject, not this rotation model's   *)
(* (see that model's header for the recovery handling of that window).      *)
(*                                                                         *)
(* The real append REFUSES (Failed, no fence) when the active key is not    *)
(* loadable (durable_audit_sink.hpp:393-396) -- modeled by the guard: the   *)
(* append simply cannot happen with an unloaded key.                        *)
(***************************************************************************)
AppendFirst ==
    /\ phase = "Running"
    /\ ~sink_fenced
    /\ main_log_empty
    /\ active_key \in ring_keys
    /\ main_log_empty' = FALSE
    /\ main_frames_signed_by' = active_key
    /\ main_anchor_signed_by' = active_key
    /\ UNCHANGED <<phase, active_key, configured_active_key_id, ring_keys,
                   sidecar_frame_durable, sidecar_anchor_durable, last_rotation,
                   sink_fenced, rotation_fenced, io_failure_injected,
                   crashes_left, repaired_without_proof, flipped>>

(***************************************************************************)
(* rotate_active_key()'s step 1a: durable sidecar evidence (KeyRotated     *)
(* frame, signed under the NEW key). The "prepare" precondition -- the new  *)
(* key must already be loaded in the ring -- is the real gate at            *)
(* durable_audit_sink.hpp:507.                                              *)
(***************************************************************************)
RotateStep1Frame ==
    /\ phase = "Running"
    /\ ~sink_fenced
    /\ ~rotation_fenced
    /\ ~sidecar_frame_durable
    \* The model's single rotation is old -> new, so it must start from the
    \* old active key (the real code derives old_key_id from the current
    \* active_key_id_, durable_audit_sink.hpp:510; a degenerate same-key
    \* "rotation" is outside this model's single-claim abstraction).
    /\ active_key = "old"
    /\ "new" \in ring_keys
    /\ \/ \* I/O failure of append_and_fsync: rotation fenced, no evidence
            \* written, nothing flips (lines 519-523).
           /\ ~io_failure_injected
           /\ io_failure_injected' = TRUE
           /\ rotation_fenced' = TRUE
           /\ UNCHANGED <<sidecar_frame_durable, last_rotation>>
       \/ \* success: the frame lands durably.
           /\ sidecar_frame_durable' = TRUE
           /\ last_rotation' = {<<"old", "new">>}
           /\ UNCHANGED <<io_failure_injected, rotation_fenced>>
    /\ UNCHANGED <<phase, active_key, configured_active_key_id, ring_keys,
                   main_log_empty, main_frames_signed_by, main_anchor_signed_by,
                   sidecar_anchor_durable, sink_fenced, crashes_left,
                   repaired_without_proof, flipped>>

(***************************************************************************)
(* rotate_active_key()'s step 1b: the sidecar's own tip anchor (fsync #2    *)
(* of step 1). A crash between 1a and 1b leaves the sidecar frame durable    *)
(* with no sidecar anchor -- a torn sidecar (lines 527-532).                *)
(***************************************************************************)
RotateStep1Anchor ==
    /\ phase = "Running"
    /\ ~sink_fenced
    /\ ~rotation_fenced
    /\ sidecar_frame_durable
    /\ ~sidecar_anchor_durable
    /\ \/ \* I/O failure of the sidecar anchor write: rotation fenced; the
            \* frame stays durable (torn sidecar on disk) (lines 528-532).
           /\ ~io_failure_injected
           /\ io_failure_injected' = TRUE
           /\ rotation_fenced' = TRUE
           /\ UNCHANGED sidecar_anchor_durable
       \/ \* success
           /\ sidecar_anchor_durable' = TRUE
           /\ UNCHANGED <<io_failure_injected, rotation_fenced>>
    /\ UNCHANGED <<phase, active_key, configured_active_key_id, ring_keys,
                   main_log_empty, main_frames_signed_by, main_anchor_signed_by,
                   sidecar_frame_durable, last_rotation, sink_fenced, crashes_left,
                   repaired_without_proof, flipped>>

(***************************************************************************)
(* rotate_active_key()'s step 2: re-anchor the MAIN log tip under the new   *)
(* key, then flip active_key_id_ (lines 540-549). Three outcomes:           *)
(*  - success: re-anchor + flip (in reality one function call; a crash      *)
(*    between them is unobservable -- the in-memory flip dies with the       *)
(*    process and the durable anchor is what a restart sees),                *)
(*  - I/O failure: the MAIN sink fences, nothing flips (line 541-545),       *)
(*  - TORN WRITE: the process dies inside write_tip_anchor; the anchor       *)
(*    file is left unreadable. This IS a crash (consumes the budget) and     *)
(*    leaves main_anchor_signed_by = "none" -- the code has no legal         *)
(*    partial-write state to be lenient about (lines 804-809).               *)
(***************************************************************************)
RotateStep2 ==
    /\ phase = "Running"
    /\ ~sink_fenced
    /\ ~rotation_fenced
    /\ sidecar_frame_durable
    /\ sidecar_anchor_durable
    /\ ~main_log_empty
    /\ main_anchor_signed_by = "old"
    /\ \/ \* success: re-anchor + flip.
           /\ main_anchor_signed_by' = "new"
           /\ active_key' = "new"
           /\ flipped' = TRUE
           /\ UNCHANGED <<phase, crashes_left, io_failure_injected, rotation_fenced,
                           sink_fenced>>
       \/ \* I/O failure: main sink fences, no flip.
           /\ ~io_failure_injected
           /\ io_failure_injected' = TRUE
           /\ sink_fenced' = TRUE
           /\ UNCHANGED <<phase, crashes_left, main_anchor_signed_by, active_key,
                           rotation_fenced, flipped>>
       \/ \* torn anchor write (a crash inside step 2).
           /\ crashes_left > 0
           /\ crashes_left' = crashes_left - 1
           /\ phase' = "Crashed"
           /\ main_anchor_signed_by' = "none"
           /\ UNCHANGED <<active_key, io_failure_injected, rotation_fenced,
                           sink_fenced, flipped>>
    /\ UNCHANGED <<configured_active_key_id, ring_keys, main_log_empty,
                   main_frames_signed_by, sidecar_frame_durable,
                   sidecar_anchor_durable, last_rotation, repaired_without_proof>>

(***************************************************************************)
(* The empty-log rotation completion: step 2 is LEGALLY SKIPPED (nothing    *)
(* to re-anchor, lines 536-546); only the durable-before-flip flip remains. *)
(* An empty main log always recovers Clean -- this must never be confused   *)
(* with an interrupted rotation.                                            *)
(***************************************************************************)
RotateCompleteOnEmptyLog ==
    /\ phase = "Running"
    /\ ~sink_fenced
    /\ ~rotation_fenced
    /\ sidecar_frame_durable
    /\ sidecar_anchor_durable
    /\ main_log_empty
    /\ active_key = "old"
    /\ active_key' = "new"
    /\ flipped' = TRUE
    /\ UNCHANGED <<phase, configured_active_key_id, ring_keys, main_log_empty,
                   main_frames_signed_by, main_anchor_signed_by, sidecar_frame_durable,
                   sidecar_anchor_durable, last_rotation, rotation_fenced,
                   io_failure_injected, crashes_left, sink_fenced,
                   repaired_without_proof>>

Next ==
    \/ RecoveryScan
    \/ Crash
    \/ AppendFirst
    \/ RotateStep1Frame
    \/ RotateStep1Anchor
    \/ RotateStep2
    \/ RotateCompleteOnEmptyLog

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* SAFETY                                                                  *)
(***************************************************************************)

\* THE INVARIANT THE DEFECT ENDANGERED. Recovery may re-anchor the main tip
\* under the new key ONLY when (a)(b)(c) all hold -- the sidecar's durable
\* proof that this exact rotation was interrupted mid-step-2. Every other
\* mismatch combo must fail closed (Corrupt + fence), never auto-repair.
RepairOnlyOnProvenMismatch == ~repaired_without_proof

\* KEY-RETIRE-009: "the active key flipped" is not "the old key is gone".
\* A running, unfenced process must be able to verify every key its main
\* log references -- which is exactly what recovery enforces by failing
\* closed when a referenced key is not loadable. (The sidecar's key is
\* deliberately excluded: an unloadable sidecar key only fences rotations,
\* never the main log -- MainLogReferencedKeys documents why.)
NoDataUnverifiableAfterRotation ==
    (phase = "Running" /\ ~sink_fenced) => MainLogReferencedKeys \subseteq ring_keys

\* Modeling-sanity: every injected I/O failure lands in a fence (step-1
\* failures -> rotation fence, step-2 failures -> main fence). Catches a
\* mis-edited failure branch.
IoFailureFencesRotationOrSink ==
    io_failure_injected => (rotation_fenced \/ sink_fenced)

\* Modeling-sanity: the durable-before-flip discipline (durable_audit_sink.hpp:
\* 480-496) -- in any process generation that actually performed the flip, the
\* sidecar evidence AND its anchor were already durable AND either the main log
\* was still empty (legal step-2 skip) or the main anchor was already
\* re-anchored under the new key. `flipped` is per-generation (reset at each
\* RecoveryScan): the constructor starting with the CONFIGURED key is not a
\* flip, and a previous generation's flip must not constrain a later one (an
\* operator may legitimately reconfigure back to the old key and append under
\* it after a completed rotation).
FlipOnlyAfterStepsDurable ==
    flipped =>
        /\ sidecar_frame_durable
        /\ sidecar_anchor_durable
        /\ (main_log_empty \/ main_anchor_signed_by = "new")

(***************************************************************************)
(* LIVENESS                                                                *)
(*                                                                         *)
(* "A crash between the two steps eventually restores a consistent state"  *)
(* is a liveness claim, not an invariant: without a bounded crash budget    *)
(* and a fairness assumption on RecoveryScan it is simply false (the        *)
(* scheduler can crash forever, or never run the scan). Like               *)
(* durable_log_recovery_liveness.cfg's comment explains, a CONSTRAINT here  *)
(* would be unsound for the liveness checker (a pruned successor set makes  *)
(* TLC treat stuttering as a legal infinite behavior), so finiteness comes  *)
(* from the action guards: crashes_left bounds Crash, and every other       *)
(* variable has a finite domain. The fairness assumption WF_vars(           *)
(* RecoveryScan) corresponds to the real operating fact: a crashed process  *)
(* is restarted by the supervisor, and the constructor of DurableAuditSink  *)
(* unconditionally runs both recovery scans -- so the scan (including the   *)
(* interrupted-rotation re-anchor I/O) is eventually taken and eventually   *)
(* succeeds. RecoveryScan deterministically lands every boot in a state     *)
(* that is either Consistent or fail-closed-fenced (every mismatch branch   *)
(* above ends in repair or in a fence; an empty log is trivially            *)
(* consistent), so the property below holds.                                *)
(***************************************************************************)

Consistent == main_log_empty \/ main_anchor_signed_by = configured_active_key_id

EventualConsistencyOrFenced ==
    [](phase = "Crashed" => <>(phase = "Running" /\ (Consistent \/ sink_fenced)))

Fairness ==
    WF_vars(RecoveryScan)

SpecLive == Init /\ [][Next]_vars /\ Fairness

=============================================================================
