----------------------- MODULE depth_snapshot_bootstrap -----------------------
(***************************************************************************)
(* MD-BOOK-002: Binance depth snapshot bootstrap state machine.            *)
(*                                                                         *)
(* WHY THIS MODEL EXISTS. formal/README.md's "Still not modeled" section   *)
(* named the depth-snapshot bootstrap as one of the three unmodeled        *)
(* subsystems that came out of the full-repo audit with real findings.     *)
(* The sequence protocol -- snapshot lastUpdateId vs the U/u windows of     *)
(* WS depthUpdate events -- is exactly the kind of continuity property      *)
(* that hand-picked unit tests reach only by construction: the model        *)
(* exhaustively checks every arrival/snapshot/gap/overflow interleaving.   *)
(*                                                                         *)
(* THE REAL CODE (native/include/hengyuan/depth_manager.hpp). DepthManager *)
(* is a single-owner, synchronous, in-memory state machine -- no crashes,  *)
(* no concurrency, no persistence (so nothing here is a crash-safety       *)
(* model; the review's "do not invent concurrency" boundary is honored --  *)
(* the SnapshotRefreshGate's worker/mailbox is a separate subsystem that    *)
(* the gate's own header declares out of scope for TLA+ modeling, and this  *)
(* model abstracts the whole fetch+apply cycle to one atomic action).      *)
(*                                                                         *)
(*   DepthState: Buffering -> (snapshot) -> Syncing -> (replay) ->          *)
(*                Tracking, with start_buffering() (resync) back to         *)
(*                Buffering from anywhere.                                  *)
(*                                                                         *)
(*   on_depth_event(ev, U, u) (lines 101-142):                             *)
(*     Buffering: buffer the event (bounded by kMaxBuffered=4096); when     *)
(*                the buffer is full, set buf_overflowed_ and drop.         *)
(*     Tracking:  gap check -- last_applied_u_ > 0 AND U > last_applied_u_  *)
(*                + 1 -> stats + start_buffering() + RE-BUFFER THIS EVENT   *)
(*                (the event itself is never discarded); otherwise apply,   *)
(*                record OrderBook::apply_delta()'s outcome, and ALWAYS     *)
(*                advance last_applied_u_ = u -- even on OutsideWindow /    *)
(*                InvalidInput (record_apply_result(), lines 210-221,       *)
(*                deliberately does not change control flow).               *)
(*                                                                         *)
(*   apply_snapshot(snap) (lines 147-202): requires Buffering. If           *)
(*     buf_overflowed_ -> resync, snapshot rejected (NOT counted).          *)
(*     Otherwise: state=Syncing, book := snapshot, last_applied_u_ =        *)
(*     snap.last_update_id, then replay the buffer IN ORDER:                *)
(*       - drop events with u <= lastUpdateId (stale),                      *)
(*       - the FIRST kept event must bridge: U <= lastUpdateId+1, else      *)
(*         resync (reject the snapshot attempt),                            *)
(*       - apply the kept events; last_applied_u_ = u each.                 *)
(*     Then buffer cleared, state=Tracking.                                 *)
(*                                                                         *)
(* MODELING CONVENTIONS                                                    *)
(*                                                                         *)
(*   The book's price/qty content is abstracted away: every invariant here  *)
(*   is about the SEQUENCE protocol, so an event is just its [U,u] window.  *)
(*   OrderBook::apply_delta()'s outcome is modeled as nondeterministic      *)
(*   (Ok / OutsideWindow / InvalidInput) -- the outcome never affects       *)
(*   control flow in the real code, only stats. The top-N window semantics  *)
(*   themselves (the other half of MD-BOOK-002) are unit-tested directly    *)
(*   (test_orderbook.cpp) and are not a sequence property; they are out of  *)
(*   this model's resolution.                                               *)
(*                                                                         *)
(*   book_consistent is the reference-sequence comparison the review        *)
(*   demands: it is TRUE iff no event has been dropped or lost in the       *)
(*   current buffering episode (episode_dropped = FALSE). The reference     *)
(*   implementation "never drops, never misorders", so a model that         *)
(*   dropped an event has a book the reference's book diverges from --      *)
(*   entering Tracking with such a book is exactly what the three           *)
(*   regression controls below reintroduce.                                 *)
(*                                                                         *)
(*   The stream is ordered (u never decreases -- a Binance guarantee);      *)
(*   gaps (U > previous u + 1) ARE possible -- a broken feed is precisely   *)
(*   what the gap detection exists for. MaxDeliveries is a modeling budget  *)
(*   (the same role as MaxCrashes in durable_log_recovery.tla), NOT a       *)
(*   design bound: it makes the state graph finite by GUARD so the liveness *)
(*   config needs no CONSTRAINT (the durable_log_recovery_liveness.cfg      *)
(*   comment explains why a CONSTRAINT would make the liveness checker      *)
(*   unsound).                                                              *)
(*                                                                         *)
(* REGRESSION CONTROLS (one per real protection mechanism):                *)
(*   1. BufferEventsInBuffering=FALSE -- the drop-before-snapshot bug:      *)
(*      events arriving during Buffering are discarded instead of buffered  *)
(*      for replay. MUST violate TrackingImpliesBookConsistent.             *)
(*   2. RejectSnapshotOnOverflow=FALSE -- the overflow-still-tracks bug:    *)
(*      apply_snapshot proceeds despite a known-incomplete buffered         *)
(*      prefix. MUST violate NoOverflowThenTracking.                        *)
(*   3. CheckFirstBridge=FALSE -- the bridge-check-disabled bug: entering   *)
(*      Tracking without verifying U <= lastUpdateId+1 on the first kept    *)
(*      event. MUST violate FirstBridgeConstraint.                          *)
(*                                                                         *)
(* LIVENESS. "A Buffering episode eventually reaches Tracking with a        *)
(* consistent book" is only true under fairness on the fetch: WF_vars(      *)
(* ApplySnapshot) says the caller's fetch loop eventually completes a       *)
(* fetch+apply. The real mechanism: the hot thread polls                    *)
(* needs_snapshot() every iteration and SnapshotRefreshGate (snapshot_      *)
(* refresh_gate.hpp) fetches asynchronously, retrying after each failure    *)
(* (finite kCooldown); assuming the exchange eventually answers is the      *)
(* same class of operational assumption as task A's supervisor-restart.     *)
(* The delivery budget makes the graph finite without a CONSTRAINT, and     *)
(* every fetch-reject / bridge-resync loop terminates because each such     *)
(* cycle consumes the delivery budget.                                      *)
(***************************************************************************)
EXTENDS Naturals, Sequences

CONSTANTS
    MaxSeq,                  \* update-id domain: events and snapshots live in 0..MaxSeq
    MaxBufferLen,            \* buffer capacity (kMaxBuffered = 4096, scaled down)
    MaxDeliveries,           \* modeling budget: total stream deliveries per run
    BufferEventsInBuffering, \* MODEL SWITCH: TRUE = current design; FALSE = drop-before-snapshot bug
    RejectSnapshotOnOverflow,\* MODEL SWITCH: TRUE = current design; FALSE = overflow-still-tracks bug
    CheckFirstBridge         \* MODEL SWITCH: TRUE = current design; FALSE = bridge-check-disabled bug

ASSUME MaxSeq \in Nat
ASSUME MaxBufferLen \in Nat
ASSUME MaxDeliveries \in Nat
ASSUME BufferEventsInBuffering \in BOOLEAN
ASSUME RejectSnapshotOnOverflow \in BOOLEAN
ASSUME CheckFirstBridge \in BOOLEAN

States == {"Buffering", "Syncing", "Tracking"}
\* A depthUpdate window: U = first update id covered, u = last (U <= u).
EventRecs == {r \in [U: 1..MaxSeq, u: 1..MaxSeq] : r.U =< r.u}
ApplyOutcomes == {"Ok", "OutsideWindow", "InvalidInput"}

VARIABLES
    state,               \* DepthState
    buffer,              \* ordered event buffer: Seq(EventRecs), Len =< MaxBufferLen.
                         \* ORDER MATTERS -- the replay walks it in arrival order.
    buffer_overflowed,   \* buf_overflowed_: an event was dropped while the
                         \* buffer was full THIS episode
    snapshot_last_u,     \* the applied snapshot's lastUpdateId (frozen at
                         \* apply_snapshot; the replay's stale-drop bound)
    last_applied_u,      \* last_applied_u_: u of the last successfully applied
                         \* event (0 before any; the snapshot sets it first)
    last_delivered_u,    \* stream position: the largest u seen so far
                         \* (the stream is ordered: u never decreases)
    deliveries_left,     \* modeling budget (action guard, not CONSTRAINT)
    episode_dropped,     \* ghost: an event was dropped or lost during the
                         \* current buffering episode (drop-bug or overflow)
    first_applied,       \* ghost: the first event the replay actually applied
                         \* (the empty set if the replay applied nothing) --
                         \* the FirstBridgeConstraint observation point. A
                         \* subset rather than a "none" sentinel: TLC refuses
                         \* to fingerprint a string compared against a record
                         \* (README trap #7).
    last_applied_event,  \* ghost: the last event applied (any path); the
                         \* empty set after an episode reset
    last_apply_outcome,  \* ghost: OrderBook::apply_delta()'s outcome for the
                         \* last apply ("none" after an episode reset)
    gap_detected         \* ghost: the most recent delivery was a Tracking gap
                         \* that triggered the re-buffer resync

vars == <<state, buffer, buffer_overflowed, snapshot_last_u, last_applied_u,
          last_delivered_u, deliveries_left, episode_dropped, first_applied,
          last_applied_event, last_apply_outcome, gap_detected>>

\* The reference-sequence comparison: TRUE iff nothing was dropped or lost in
\* the current episode, i.e. the book (built from the snapshot + the applied
\* events) is exactly what a never-drop, never-misorder implementation would
\* have built from the same stream.
book_consistent == ~episode_dropped

TypeOK ==
    /\ state \in States
    /\ buffer \in Seq(EventRecs)
    /\ Len(buffer) =< MaxBufferLen
    /\ buffer_overflowed \in BOOLEAN
    /\ snapshot_last_u \in 0..MaxSeq
    /\ last_applied_u \in 0..MaxSeq
    /\ last_delivered_u \in 0..MaxSeq
    /\ deliveries_left \in 0..MaxDeliveries
    /\ episode_dropped \in BOOLEAN
    /\ first_applied \subseteq EventRecs
    /\ last_applied_event \subseteq EventRecs
    /\ last_apply_outcome \in {"none"} \cup ApplyOutcomes
    /\ gap_detected \in BOOLEAN

Init ==
    \* A fresh DepthManager starts in Buffering with an empty book and no
    \* snapshot (depth_manager.hpp:223-233; test StartsInBufferingState).
    /\ state = "Buffering"
    /\ buffer = <<>>
    /\ buffer_overflowed = FALSE
    /\ snapshot_last_u = 0
    /\ last_applied_u = 0
    /\ last_delivered_u = 0
    /\ deliveries_left = MaxDeliveries
    /\ episode_dropped = FALSE
    /\ first_applied = {}
    /\ last_applied_event = {}
    /\ last_apply_outcome = "none"
    /\ gap_detected = FALSE

(***************************************************************************)
(* One WS depthUpdate event arrives AND is processed (single-owner          *)
(* synchronous: arrival and handling are one step; the review's "do not    *)
(* invent a mailbox" boundary).                                             *)
(***************************************************************************)
DeliverEvent(e) ==
    /\ e \in EventRecs
    /\ e.u >= last_delivered_u    \* the stream is ordered (Binance guarantee)
    /\ deliveries_left > 0
    /\ deliveries_left' = deliveries_left - 1
    /\ last_delivered_u' = e.u
    /\ UNCHANGED <<snapshot_last_u, first_applied>>
    /\ IF state = "Buffering" THEN
           IF Len(buffer) < MaxBufferLen THEN
               IF BufferEventsInBuffering THEN
                   \* current: buffer it for replay (lines 104-110).
                   /\ buffer' = Append(buffer, e)
                   /\ UNCHANGED <<state, buffer_overflowed, last_applied_u,
                                   episode_dropped, last_applied_event,
                                   last_apply_outcome, gap_detected>>
               ELSE
                   \* DROP-BEFORE-SNAPSHOT BUG: the event is discarded. The
                   \* book has now diverged from the reference, which buffered
                   \* it -- the replay can never detect the hole (the real
                   \* replay has no mid-buffer continuity check after the
                   \* first kept event, lines 172-197).
                   /\ buffer' = buffer
                   /\ episode_dropped' = TRUE
                   /\ UNCHANGED <<state, buffer_overflowed, last_applied_u,
                                   last_applied_event, last_apply_outcome,
                                   gap_detected>>
           ELSE
               \* buffer full: the event is lost and the overflow is recorded
               \* (lines 111-120). The buffered prefix is known-incomplete.
               /\ buffer' = buffer
               /\ buffer_overflowed' = TRUE
               /\ episode_dropped' = TRUE
               /\ UNCHANGED <<state, last_applied_u, last_applied_event,
                               last_apply_outcome, gap_detected>>
       ELSE IF state = "Tracking" THEN
           IF last_applied_u > 0 /\ e.U > last_applied_u + 1 THEN
               \* GAP (lines 124-133): start_buffering() -- which clears the
               \* book, the buffer, the overflow flag and last_applied_u_ --
               \* then RE-BUFFER THIS EVENT (the recursive on_depth_event
               \* call). The gap event is never discarded.
               /\ state' = "Buffering"
               /\ buffer' = <<e>>
               /\ buffer_overflowed' = FALSE
               /\ last_applied_u' = 0
                /\ episode_dropped' = FALSE
                /\ last_applied_event' = {}
                /\ last_apply_outcome' = "none"
                /\ gap_detected' = TRUE
           ELSE
               \* apply (lines 135-138): consume the sequence REGARDLESS of
               \* the book-layer outcome (record_apply_result, lines 210-221
               \* -- OutsideWindow / InvalidInput must still advance the
               \* sequence, else the next event would manufacture a false
               \* gap).
                /\ last_applied_u' = e.u
                /\ last_applied_event' = {e}
                /\ last_apply_outcome' \in ApplyOutcomes
               /\ gap_detected' = FALSE
               /\ UNCHANGED <<state, buffer, buffer_overflowed, episode_dropped>>
       ELSE
           \* Syncing: on_depth_event() returns false without doing anything
           \* (the real replay is atomic; see the header). Unreachable in
           \* this model, kept for faithfulness of the code's branch shape.
           /\ UNCHANGED <<state, buffer, buffer_overflowed, last_applied_u,
                           episode_dropped, last_applied_event, last_apply_outcome,
                           gap_detected>>

\* The replay loop (lines 168-201), as one atomic step: drop stale events,
\* verify the first kept event bridges, apply the kept suffix in order,
\* clear the buffer, enter Tracking -- or resync on a bridge failure.
ReplayAfterSnapshot(S) ==
    LET kept == SelectSeq(buffer, LAMBDA e : e.u > S) IN
    IF kept = <<>> THEN
        \* every buffered event is stale (u <= lastUpdateId): all dropped, the
        \* book is exactly the snapshot (test DropsBufferedEventsBeforeSnapshot).
        /\ state' = "Tracking"
        /\ buffer' = <<>>
        /\ last_applied_u' = S
        /\ first_applied' = {}
        /\ episode_dropped' = episode_dropped
        /\ UNCHANGED <<buffer_overflowed, last_applied_event, last_apply_outcome>>
    ELSE
        LET e1 == kept[1] IN
        IF CheckFirstBridge /\ e1.U > S + 1 THEN
            \* BRIDGE FAILURE: the first kept event has a gap right after the
            \* snapshot (U > lastUpdateId+1) -- resync (lines 181-189; test
            \* ResyncOnGapInBufferedEvents).
            /\ state' = "Buffering"
            /\ buffer' = <<>>
            /\ buffer_overflowed' = FALSE
            /\ last_applied_u' = 0
            /\ episode_dropped' = FALSE    \* start_buffering: fresh episode
            /\ UNCHANGED <<first_applied, last_applied_event, last_apply_outcome>>
        ELSE
            \* apply the kept suffix in order (the replay loop, lines 192-197);
            \* last_applied_u advances to the LAST kept event's u.
            /\ state' = "Tracking"
            /\ buffer' = <<>>
            /\ last_applied_u' = kept[Len(kept)].u
            /\ first_applied' = {e1}
            /\ last_applied_event' = {kept[Len(kept)]}
            /\ last_apply_outcome' \in ApplyOutcomes
            /\ episode_dropped' = episode_dropped
            /\ UNCHANGED buffer_overflowed

(***************************************************************************)
(* The caller's snapshot fetch completes and the snapshot is applied        *)
(* (apply_snapshot(), lines 147-202). The REST round trip (SnapshotRefresh- *)
(* Gate's Idle/InFlight/Cooldown) is abstracted to this one action: the     *)
(* gate's state machine is about fetch THROUGHPUT, not sequence semantics,  *)
(* and its own header defers TLA+ modeling of it to a future round.         *)
(*                                                                         *)
(* S (the snapshot's lastUpdateId) is an environment choice: the exchange   *)
(* may return a snapshot fresher or staler than the stream position -- a    *)
(* stale snapshot is exactly what makes the bridge check bite.              *)
(***************************************************************************)
ApplySnapshot(S) ==
    /\ state = "Buffering"
    /\ S \in 0..MaxSeq
    /\ snapshot_last_u' = S
    /\ gap_detected' = FALSE
    /\ IF buffer_overflowed THEN
           IF RejectSnapshotOnOverflow THEN
               \* current: the buffered prefix is known-incomplete -- discard
               \* this snapshot attempt and force a fresh buffering episode
               \* (lines 150-158; test OverflowedBufferForcesResyncInsteadOfTracking).
               /\ state' = "Buffering"
               /\ buffer' = <<>>
               /\ buffer_overflowed' = FALSE
               /\ last_applied_u' = 0
               /\ episode_dropped' = FALSE    \* fresh episode
               /\ UNCHANGED <<first_applied, last_applied_event, last_apply_outcome>>
           ELSE
               \* OVERFLOW-STILL-TRACKS BUG: proceed despite the known gap.
               /\ ReplayAfterSnapshot(S)
       ELSE
           /\ ReplayAfterSnapshot(S)
    /\ UNCHANGED <<last_delivered_u, deliveries_left>>

(***************************************************************************)
(* An external resync request (the hot thread's resync path). Same effect   *)
(* as the internal start_buffering() calls, exposed as an explicit action   *)
(* so the caller-side resync is also explored.                              *)
(***************************************************************************)
StartBuffering ==
    /\ state' = "Buffering"
    /\ buffer' = <<>>
    /\ buffer_overflowed' = FALSE
    /\ last_applied_u' = 0
    /\ episode_dropped' = FALSE
    /\ last_applied_event' = {}
    /\ last_apply_outcome' = "none"
    /\ gap_detected' = FALSE
    /\ UNCHANGED <<snapshot_last_u, first_applied, last_delivered_u,
                   deliveries_left>>

Next ==
    \/ \E e \in EventRecs : DeliverEvent(e)
    \/ \E S \in 0..MaxSeq : ApplySnapshot(S)
    \/ StartBuffering

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* SAFETY                                                                  *)
(***************************************************************************)

\* 1. A snapshot applied over a known-incomplete buffered prefix must be
\* rejected -- the state machine must go back to Buffering, never enter
\* Tracking with the overflow flag still set (depth_manager.hpp:150-158;
\* test OverflowedBufferForcesResyncInsteadOfTracking).
NoOverflowThenTracking ==
    state = "Tracking" => ~buffer_overflowed

\* 2. Stale increments (u <= snapshot_last_u) are dropped at replay, never
\* applied: the first applied event of a replay must carry u > lastUpdateId
\* (the per-event drop check, lines 176-179; test DropsBufferedEventsBeforeSnapshot).
StaleIncrementsDropped ==
    state = "Tracking" =>
        (first_applied = {} \/ \E e \in first_applied : e.u > snapshot_last_u)

\* 3. The transition into Tracking is bridged: the first kept, actually
\* applied increment satisfies U <= lastUpdateId+1 <= u -- otherwise the
\* snapshot attempt is rejected and the machine resyncs (lines 181-189;
\* test ResyncOnGapInBufferedEvents). `<= u` follows from the keep check
\* (u > lastUpdateId means u >= lastUpdateId+1).
FirstBridgeConstraint ==
    state = "Tracking" =>
        (first_applied = {}
         \/ \E e \in first_applied :
             (e.U =< snapshot_last_u + 1 /\ snapshot_last_u + 1 =< e.u))

\* 4. A gap detected while Tracking must return to Buffering WITH the gap
\* event itself re-buffered -- never discarded (lines 124-133). gap_detected
\* is the observation point for the resync transition; the invariant pins
\* the post-state of that transition.
TrackingGapForcesRebuffer ==
    gap_detected => (state = "Buffering" /\ buffer # <<>> /\ last_applied_u = 0)

\* 5. OrderBook::apply_delta() rejecting an event (OutsideWindow or
\* InvalidInput) must not skip the sequence: the event is still consumed,
\* last_applied_u advances to its u (record_apply_result + lines 135-138).
\* Consuming less would manufacture a false gap on the very next event.
SequenceAlwaysConsumedOnRejection ==
    (state = "Tracking" /\ last_apply_outcome \in {"OutsideWindow", "InvalidInput"}
     /\ last_applied_event # {}) =>
        \E e \in last_applied_event : last_applied_u = e.u

\* Bootstrap-complete book consistency: entering Tracking requires the book
\* to match the never-drop reference (episode_dropped = FALSE). Violated by
\* the drop-before-snapshot bug (and, as a side effect, the overflow bug).
TrackingImpliesBookConsistent ==
    state = "Tracking" => book_consistent

(***************************************************************************)
(* LIVENESS                                                                *)
(*                                                                         *)
(* "A Buffering episode eventually reaches Tracking with a consistent       *)
(* book" is a liveness claim: the scheduler can stutter in Buffering        *)
(* forever. Like durable_log_recovery_liveness.cfg, this config MUST NOT    *)
(* use a CONSTRAINT -- a pruned successor set makes TLC treat stuttering    *)
(* as a legal infinite behavior and report a spurious violation. Finiteness *)
(* comes from the action guards: deliveries_left bounds DeliverEvent, and   *)
(* every other variable has a finite domain. The only fairness assumption   *)
(* is WF_vars(ApplySnapshot): the caller's fetch loop (hot thread polling   *)
(* needs_snapshot() every iteration, SnapshotRefreshGate retrying after     *)
(* each failure's finite cooldown) eventually completes a fetch+apply.      *)
(* Every fetch-reject / bridge-resync cycle consumes the delivery budget,   *)
(* so after finitely many cycles the buffer is empty or bridging and the    *)
(* fetch lands in Tracking.                                                 *)
(***************************************************************************)

EventualBootstrap ==
    [](state = "Buffering" => <>(state = "Tracking" /\ book_consistent))

Fairness ==
    \* Per snapshot value, never WF_vars(\E S : ApplySnapshot(S)) -- the same
    \* per-instance discipline as every other model in this directory (a \E
    \* fairness only promises that SOME value's fetch completes).
    \A S \in 0..MaxSeq : WF_vars(ApplySnapshot(S))

SpecLive == Init /\ [][Next]_vars /\ Fairness

=============================================================================
