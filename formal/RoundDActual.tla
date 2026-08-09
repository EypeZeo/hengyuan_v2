------------------------------ MODULE RoundDActual ------------------------------
(***************************************************************************)
(* Models the REAL, shipped public API of Round D's CandidateLease /       *)
(* IntentStore (native/include/hengyuan/compaction_lease.hpp,              *)
(* compaction_intent_store.hpp -- docs/SPEC_INVARIANTS.md's "Seal-journal  *)
(* Round D" entry), not a generic file-lease design. Deliberately narrow:  *)
(* CreateGenesis is the ONLY filesystem write in the state space -- there  *)
(* is no generalized "artifact name" parameter anywhere here, matching the *)
(* real API's own shape (compaction_lease.hpp's create_intent_genesis_no_  *)
(* replace() takes no filename argument at all; it is compile-time fixed). *)
(*                                                                         *)
(* WHY THIS MODEL EXISTS. Six external Architect reviews rejected five     *)
(* successive designs before the shipped one (see compaction_lease.hpp's   *)
(* own top-of-file comment for the full history). Three of those findings  *)
(* are safety properties with a real, reachable counterexample in an       *)
(* earlier design -- not style opinions -- which makes them exactly the    *)
(* kind of thing this repo's existing models (durable_log_recovery.tla,    *)
(* freeze_episode_recovery.tla, inflight_lifecycle.tla) already argue is   *)
(* worth a regression control: a MODEL SWITCH that reintroduces the        *)
(* rejected design and MUST make the corresponding invariant fail. If any  *)
(* of the three ever stops failing, the model has stopped discriminating   *)
(* the finding it exists to prove, exactly as those three files' own       *)
(* README sections already argue for their own controls.                  *)
(*                                                                         *)
(*   v3Bug   (HandleRelativeIo=FALSE): the design before handle-relative   *)
(*           I/O -- check identity by path, then separately open the       *)
(*           artifact by path again. TOCTOU: an external rename/recreate   *)
(*           between the two steps redirects the write into a directory    *)
(*           the caller no longer controls.                                *)
(*   v4Bug   (HandleEncapsulated=FALSE): a public dir_handle() accessor    *)
(*           let a caller save a reference past the lease's own lifetime   *)
(*           and use it after release()/fencing.                           *)
(*   v5Bug   (OwnerThreadCheckedOnRelease=FALSE): release() had no         *)
(*           owner-thread check, so a non-owner thread could clear `held`  *)
(*           (and, in the real code, close the handle) while the owner is  *)
(*           still using it.                                               *)
(*                                                                         *)
(* SCOPE. This model covers exactly Round D's shipped write surface:       *)
(* acquire/release lifecycle, sticky directory-identity fencing, and       *)
(* genesis-only, no-replace, handle-relative writes. It does NOT model     *)
(* KeyRing pin/retire (a separate subsystem with its own real coverage --  *)
(* test_key_ring.cpp's KeyRingDestroyedWithLivePinTerminates death test    *)
(* already exercises that fail-closed property directly), the `.x1`/`.xgc` *)
(* phase-raise/GC state machine (Round D never writes either -- see        *)
(* RoundEFDesign.tla, a genuinely separate, NOT-yet-implemented future     *)
(* design with no production traceability of its own), or cross-process    *)
(* mutual exclusion (an OS-level guarantee, exercised for real by          *)
(* test_compaction_lease.cpp's CrossProcessMutualExclusion test against an *)
(* actual second process -- not something a single-process TLA+ model adds *)
(* confidence to).                                                         *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS
    MaxCreateAttempts,         \* bounds CreateGenesis calls for state-space finiteness
    HandleRelativeIo,          \* MODEL SWITCH: TRUE = current design (v4+); FALSE = v3Bug
    HandleEncapsulated,        \* MODEL SWITCH: TRUE = current design; FALSE = v4Bug
    OwnerThreadCheckedOnRelease \* MODEL SWITCH: TRUE = current design; FALSE = v5Bug

VARIABLES
    held,                      \* BOOLEAN -- CandidateLease::held_
    fenced,                    \* BOOLEAN -- CandidateLease::fenced_ (sticky: TRUE never reverts)
    dirIdentityChanged,        \* BOOLEAN -- ground truth: has the directory been externally
                               \*   renamed/recreated since acquire()? (an environment fact,
                               \*   not something the lease necessarily knows about yet)
    genesisWritten,            \* BOOLEAN -- has the genesis record actually landed
    createAttempts,            \* Nat, bounded by MaxCreateAttempts
    wroteIntoWrongObject,      \* ghost: TRUE iff a write ever landed while
                               \*   dirIdentityChanged was true and undetected (the v3Bug signature)
    handleCapabilityIssued,    \* ghost: TRUE iff HandleEncapsulated=FALSE and some caller
                               \*   obtained a reference to the underlying handle (v4Bug precondition)
    handleUsedAfterInvalidated,\* ghost: TRUE iff that reference was used after held became
                               \*   FALSE or fenced became TRUE (the v4Bug signature)
    releasedByNonOwner         \* ghost: TRUE iff a non-owner release() call ever actually
                               \*   cleared `held` (the v5Bug signature)

vars == <<held, fenced, dirIdentityChanged, genesisWritten, createAttempts,
           wroteIntoWrongObject, handleCapabilityIssued, handleUsedAfterInvalidated,
           releasedByNonOwner>>

TypeOK ==
    /\ held \in BOOLEAN
    /\ fenced \in BOOLEAN
    /\ dirIdentityChanged \in BOOLEAN
    /\ genesisWritten \in BOOLEAN
    /\ createAttempts \in 0..MaxCreateAttempts
    /\ wroteIntoWrongObject \in BOOLEAN
    /\ handleCapabilityIssued \in BOOLEAN
    /\ handleUsedAfterInvalidated \in BOOLEAN
    /\ releasedByNonOwner \in BOOLEAN

Init ==
    /\ held = FALSE
    /\ fenced = FALSE
    /\ dirIdentityChanged = FALSE
    /\ genesisWritten = FALSE
    /\ createAttempts = 0
    /\ wroteIntoWrongObject = FALSE
    /\ handleCapabilityIssued = FALSE
    /\ handleUsedAfterInvalidated = FALSE
    /\ releasedByNonOwner = FALSE

(***************************************************************************)
(* CandidateLease::acquire() -- only reachable from a fresh (never-held,   *)
(* never-fenced) state, matching "recovery must construct a fresh          *)
(* CandidateLease against a re-audited path" (compaction_lease.hpp's own   *)
(* comment): this model does not attempt re-acquisition of an already-     *)
(* fenced lease, the same way the real API offers no such operation.       *)
(***************************************************************************)
Acquire ==
    /\ ~held
    /\ ~fenced
    /\ held' = TRUE
    /\ UNCHANGED <<fenced, dirIdentityChanged, genesisWritten, createAttempts,
                    wroteIntoWrongObject, handleCapabilityIssued,
                    handleUsedAfterInvalidated, releasedByNonOwner>>

(***************************************************************************)
(* Environment action: an external actor renames/recreates the candidate   *)
(* directory while it is held. Modeled as always possible while held --    *)
(* the design must be safe in the world where this happens (POSIX rename() *)
(* never cares about open descriptors); that Windows additionally, and     *)
(* structurally, refuses this for as long as a share-delete-less handle is *)
(* open anywhere in the subtree (found empirically while writing           *)
(* test_compaction_intent_store.cpp's IntentStoreDirectoryFencing tests)   *)
(* is a bonus property of the CURRENT implementation on ONE platform, not  *)
(* something this abstract safety model should assume away.                *)
(***************************************************************************)
ExternalIdentityChange ==
    /\ held
    /\ ~dirIdentityChanged
    /\ dirIdentityChanged' = TRUE
    /\ UNCHANGED <<held, fenced, genesisWritten, createAttempts,
                    wroteIntoWrongObject, handleCapabilityIssued,
                    handleUsedAfterInvalidated, releasedByNonOwner>>

(***************************************************************************)
(* IntentStore::create_building_intent() -> CandidateLease::create_intent_ *)
(* genesis_no_replace(). No filename parameter anywhere -- Round D's only  *)
(* write always targets the one compile-time-fixed genesis artifact.       *)
(*                                                                         *)
(* check_can_operate()'s real order: held check, (owner-thread check --    *)
(* not modeled here, see the module header's Scope note: it has no known   *)
(* safety-relevant historical bug, only a status-code misclassification    *)
(* one, see compaction_lease.hpp's own comment on bug #11), fenced check   *)
(* (sticky: refuse without re-touching the filesystem), THEN identity      *)
(* check (first detection: fence AND refuse, in the same call, before any  *)
(* write is attempted).                                                    *)
(*                                                                         *)
(* HandleRelativeIo=FALSE (v3Bug) models what a path-based (not handle-    *)
(* relative) implementation would do instead: it has no way to detect      *)
(* "the object I'm about to open by path is not the one I checked a moment *)
(* ago", so an undetected identity change lets the write proceed anyway,   *)
(* landing in whatever new object now occupies that path.                  *)
(***************************************************************************)
CreateGenesis ==
    /\ held
    /\ ~fenced
    /\ createAttempts < MaxCreateAttempts
    /\ createAttempts' = createAttempts + 1
    /\ IF dirIdentityChanged THEN
           IF HandleRelativeIo THEN
               \* current design: detect, fence, refuse -- no write.
               /\ fenced' = TRUE
               /\ UNCHANGED <<genesisWritten, wroteIntoWrongObject>>
           ELSE
               \* v3Bug: no detection mechanism: the write silently
               \* proceeds against whatever object is now at that path.
               /\ genesisWritten' = TRUE
               /\ wroteIntoWrongObject' = TRUE
               /\ UNCHANGED fenced
       ELSE
           \* identity still matches (or was never checked because it
           \* never changed) -- ordinary write-once genesis create.
           /\ genesisWritten' = TRUE
           /\ UNCHANGED <<fenced, wroteIntoWrongObject>>
    /\ UNCHANGED <<held, dirIdentityChanged, handleCapabilityIssued,
                    handleUsedAfterInvalidated, releasedByNonOwner>>

(***************************************************************************)
(* release(). Split into owner/non-owner actions (rather than one action   *)
(* parameterized by an actor identity) so the v5Bug divergence is a first- *)
(* class, separately-toggleable transition -- matching this repo's         *)
(* existing style of expressing a historical bug as a guard/assignment     *)
(* that changes under a CONSTANT switch (e.g. freeze_episode_recovery.tla's*)
(* SinkVerifiesWaitIndependently).                                        *)
(***************************************************************************)
ReleaseByOwner ==
    /\ held
    /\ held' = FALSE
    /\ UNCHANGED <<fenced, dirIdentityChanged, genesisWritten, createAttempts,
                    wroteIntoWrongObject, handleCapabilityIssued,
                    handleUsedAfterInvalidated, releasedByNonOwner>>

ReleaseByNonOwner ==
    /\ held
    /\ IF OwnerThreadCheckedOnRelease THEN
           \* current design: WrongOwner, handle/held untouched.
           /\ UNCHANGED held
           /\ UNCHANGED releasedByNonOwner
       ELSE
           \* v5Bug: no owner-thread check at all -- anyone can release.
           /\ held' = FALSE
           /\ releasedByNonOwner' = TRUE
    /\ UNCHANGED <<fenced, dirIdentityChanged, genesisWritten, createAttempts,
                    wroteIntoWrongObject, handleCapabilityIssued,
                    handleUsedAfterInvalidated>>

(***************************************************************************)
(* v4Bug-only actions (HandleEncapsulated=FALSE): a capability (the raw    *)
(* handle) escapes the lease's own encapsulation and can be used later,    *)
(* independent of the lease's own held/fenced state. In the current        *)
(* design there is no accessor that could produce such a capability at     *)
(* all -- CandidateLease's only public surface is acquire/release/held/    *)
(* fenced/last_identity_diagnostic(), none of which return anything that   *)
(* could touch a filesystem (compaction_lease.hpp's own header comment,    *)
(* point 1) -- so these actions are simply never enabled when              *)
(* HandleEncapsulated=TRUE.                                                *)
(***************************************************************************)
BorrowHandle ==
    /\ ~HandleEncapsulated
    /\ held
    /\ ~handleCapabilityIssued
    /\ handleCapabilityIssued' = TRUE
    /\ UNCHANGED <<held, fenced, dirIdentityChanged, genesisWritten, createAttempts,
                    wroteIntoWrongObject, handleUsedAfterInvalidated, releasedByNonOwner>>

UseHandleViaCapability ==
    /\ ~HandleEncapsulated
    /\ handleCapabilityIssued
    /\ IF ~held \/ fenced THEN handleUsedAfterInvalidated' = TRUE
                           ELSE UNCHANGED handleUsedAfterInvalidated
    /\ UNCHANGED <<held, fenced, dirIdentityChanged, genesisWritten, createAttempts,
                    wroteIntoWrongObject, handleCapabilityIssued, releasedByNonOwner>>

Next ==
    \/ Acquire
    \/ ExternalIdentityChange
    \/ CreateGenesis
    \/ ReleaseByOwner
    \/ ReleaseByNonOwner
    \/ BorrowHandle
    \/ UseHandleViaCapability

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* SAFETY                                                                  *)
(***************************************************************************)

(* v3Bug's signature: a write landing in a directory object whose identity *)
(* had already diverged from what acquire() captured. *)
NeverWritesIntoWrongObject == ~wroteIntoWrongObject

(* v4Bug's signature: a capability outliving the lease that issued it. *)
NoCapabilityUseAfterInvalidation == ~handleUsedAfterInvalidated

(* v5Bug's signature: a non-owner successfully clearing `held`. *)
NonOwnerReleaseNeverClearsHeld == ~releasedByNonOwner

(* Once fenced, always fenced -- true by construction (no action above     *)
(* ever assigns fenced' = FALSE), but stated as an explicit TEMPORAL       *)
(* PROPERTY (see the .cfg's PROPERTIES section) so a future edit that      *)
(* broke it would be caught by TLC rather than silently relying on         *)
(* "nothing currently does that". An INVARIANT cannot express this --      *)
(* "never reverts" is a claim about a state and all its successors, not a  *)
(* single-state predicate.                                                *)
FenceIsSticky == [](fenced => [](fenced))

(***************************************************************************)
(* LIVENESS                                                                *)
(*                                                                         *)
(* A held lease eventually reaches SOME resolution -- written, fenced, or  *)
(* released -- it does not get stuck forever with nothing able to make     *)
(* progress. Weak fairness only; ExternalIdentityChange and the two        *)
(* handle-capability actions deliberately have none (an attacker/           *)
(* environment is never assumed to act, and a non-owner is never assumed   *)
(* to try releasing).                                                      *)
(***************************************************************************)
Fairness ==
    /\ WF_vars(Acquire)
    /\ WF_vars(CreateGenesis)
    /\ WF_vars(ReleaseByOwner)

SpecLive == Init /\ [][Next]_vars /\ Fairness

EventualResolution ==
    [](held => <>(genesisWritten \/ fenced \/ ~held))

StateConstraint == createAttempts =< MaxCreateAttempts

===================================================================================
