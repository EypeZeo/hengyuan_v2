-------------------------- MODULE reconnect_sweep --------------------------
(***************************************************************************)
(* The reconnect barrier and its sweep (private-stream migration M3a/M3b,  *)
(* docs/PRIVATE_STREAM_MIGRATION_DESIGN.md 5.2-5.4, risk R9):              *)
(*                                                                         *)
(*   hot thread  : ReconnectBarrier + SweepPump + drain_reconcile_events() *)
(*   rings       : SweepRequestRing, ToReconcileRing, ReconcileEventRing   *)
(*   reconcile   : poll_once() with a SweepJob over the OrderTracker       *)
(*                                                                         *)
(* WHAT IT CHECKS (the two claims the C++ states and the tests probe):     *)
(*                                                                         *)
(*  ReconciledIsSwept   When the barrier becomes Reconciled, every order   *)
(*     the hot thread had admitted before that sweep's request was posted  *)
(*     is either already PUBLISHED as resolved on the hot thread, or was   *)
(*     confirmed still live by the sweep itself (a definite answer after   *)
(*     the request). Nothing the sweep needs to know can still be in       *)
(*     transit or unknown.                                                 *)
(*                                                                         *)
(*  TokensOnlyWhenReady A token is issued only while the barrier says      *)
(*     Reconciled AND the stream is connected on the very generation the   *)
(*     barrier swept (authorize() observes first; a disconnect or a new    *)
(*     generation drops the barrier to Down).                              *)
(*                                                                         *)
(* THE FIVE SWITCHES are the five design decisions that make those claims  *)
(* true. Each has a config that turns it OFF and MUST then fail:           *)
(*    TakeBeforeDrain   poll_once() takes the sweep request BEFORE it      *)
(*                      drains the ingress ring. (Else an order sent       *)
(*                      before the request can miss the sweep.)            *)
(*    MarkerInBand      The SweepDone marker rides the SAME FIFO ring as   *)
(*                      the resolutions. (Else a report can overtake the   *)
(*                      fill it is supposed to follow.)                    *)
(*    EpochChecked      A report is honoured only for the current epoch.   *)
(*    ObserveFirst      authorize() refreshes the barrier from the feed    *)
(*                      before deciding.                                   *)
(*    DownOnDisconnect  observe() drops to Down when the session is not    *)
(*                      connected (and on any generation change).          *)
(*                                                                         *)
(* SCOPE. Ring operations are sequentially consistent here; the memory-    *)
(* order side of those edges (release/acquire on the SPSC rings) is what   *)
(* ThreadSanitizer and the arm64 repeat run exercise, not this model. No   *)
(* time: timeouts and the account's freshness are nondeterministic. The    *)
(* escalated-to-operator count and the per-tick query budget are tested    *)
(* in test_order_tracker_sweep.cpp, not modelled: they change how long a   *)
(* sweep takes or what it reports, not the ordering the claims rest on.    *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets, Sequences

CONSTANTS
    ORDERS,            \* the orders this run may admit
    MaxGen,            \* highest stream generation
    MaxEpoch,          \* bound on the barrier's epoch counter
    MaxDrops,          \* bound on lost-event count within one session
    EvCap,             \* capacity of the reconcile event ring (and of the overtaking channel)
    ReqCap,            \* capacity of the sweep request ring
    TakeBeforeDrain,   \* SWITCH, see above
    MarkerInBand,      \* SWITCH
    EpochChecked,      \* SWITCH
    ObserveFirst,      \* SWITCH
    DownOnDisconnect   \* SWITCH

VARIABLES
    \* --- the world: the exchange and the private stream ---
    truth,      \* [ORDERS -> {"none", "open", "final"}]  the exchange's state of each order
    sgen,       \* the stream generation (0 = no session yet)
    sconn,      \* the session is connected and verified
    sdrops,     \* lost-event counter of the current session
    \* --- the hot thread ---
    admitted,   \* SUBSET ORDERS: sent (an Ambiguous submit: registry slot + ingress push)
    released,   \* SUBSET ORDERS: resolution popped from the event ring, published, slot released
    b,          \* the barrier: [st, gen, drops, ep, out, bd]; bd = what had been admitted when THIS epoch's request was posted
    \* --- the rings ---
    ingress,    \* Seq(ORDERS)  hot -> reconcile (orders)
    reqq,       \* Seq(1..MaxEpoch)  hot -> reconcile (sweep requests)
    evq,        \* Seq(Event)  reconcile -> hot (resolutions, and the markers when MarkerInBand)
    side,       \* Seq(Event)  an overtaking channel for the markers when NOT MarkerInBand
    \* --- the reconcile thread ---
    tracked,    \* SUBSET ORDERS: in the OrderTracker
    answered,   \* [ORDERS -> 0..MaxEpoch]: the sweep epoch that last answered the order
    jepoch,     \* SweepJob epoch (0 = none yet)
    jrunning,   \* a sweep was begun and its marker has not been pushed
    tick,       \* sub-step of the current poll_once() call: 0 = start, 1 = first of {take, drain} done, 2 = both done
    \* --- ghosts: monotone verdicts, checked as invariants ---
    rec_ok,     \* every acceptance of a report so far satisfied the sweep claim
    tok_ok,     \* every token so far was issued on a Reconciled barrier on the live generation
    issued      \* tokens issued (saturating at 2, only for the reachability probe)

vars == <<truth, sgen, sconn, sdrops, admitted, released, b, ingress, reqq, evq, side,
          tracked, answered, jepoch, jrunning, tick, rec_ok, tok_ok, issued>>

SeqToSet(s) == {s[i] : i \in 1..Len(s)}
SeqMax(s)   == CHOOSE m \in SeqToSet(s) : \A i \in 1..Len(s) : s[i] <= m

(***************************************************************************)
(* ReconnectBarrier. b.st in {"Down","Sweeping","Reconciled"}; ep is the   *)
(* epoch; out = a request is outstanding; bd = the orders admitted when    *)
(* the outstanding request was posted ({} until then, and again after any  *)
(* epoch change: a report is only ever judged against the current epoch).  *)
(***************************************************************************)
DownB(x)  == IF x.st = "Down" THEN x
             ELSE [x EXCEPT !.st = "Down", !.ep = @ + 1, !.out = FALSE, !.bd = {}]   \* enter_down()
SweepB(x) == [x EXCEPT !.st = "Sweeping", !.ep = @ + 1, !.out = FALSE, !.bd = {}]    \* begin_sweep()

\* ReconnectBarrier::observe(): a generation change drops the barrier; a session that is not connected
\* drops it (DownOnDisconnect); a connected session while Down starts a sweep; a lost event while up
\* restarts it (the soft barrier). The loss counter is the barrier's baseline, refreshed on every
\* connected sample.
ObserveB(x) ==
    LET x1 == IF sgen # x.gen THEN DownB([x EXCEPT !.gen = sgen]) ELSE x
    IN IF sgen = 0 \/ (DownOnDisconnect /\ ~sconn)
         THEN DownB(x1)
         ELSE LET trouble == sdrops # x1.drops
                  x2      == [x1 EXCEPT !.drops = sdrops]
              IN IF x2.st = "Down" THEN SweepB(x2)
                 ELSE IF trouble THEN SweepB(x2) ELSE x2

TypeOK ==
    /\ truth \in [ORDERS -> {"none", "open", "final"}]
    /\ sgen \in 0..MaxGen /\ sconn \in BOOLEAN /\ sdrops \in 0..MaxDrops
    /\ admitted \subseteq ORDERS /\ released \subseteq ORDERS /\ released \subseteq admitted
    /\ b.st \in {"Down", "Sweeping", "Reconciled"} /\ b.out \in BOOLEAN /\ b.bd \subseteq ORDERS
    /\ tracked \subseteq ORDERS
    /\ answered \in [ORDERS -> 0..MaxEpoch]
    /\ tick \in 0..2 /\ jrunning \in BOOLEAN
    /\ rec_ok \in BOOLEAN /\ tok_ok \in BOOLEAN /\ issued \in 0..2

Init ==
    /\ truth    = [o \in ORDERS |-> "none"]
    /\ sgen     = 0 /\ sconn = FALSE /\ sdrops = 0
    /\ admitted = {} /\ released = {}
    /\ b        = [st |-> "Down", gen |-> 0, drops |-> 0, ep |-> 0, out |-> FALSE, bd |-> {}]
    /\ ingress  = << >> /\ reqq = << >> /\ evq = << >> /\ side = << >>
    /\ tracked  = {} /\ answered = [o \in ORDERS |-> 0]
    /\ jepoch   = 0 /\ jrunning = FALSE /\ tick = 0
    /\ rec_ok   = TRUE /\ tok_ok = TRUE /\ issued = 0

(***************************************************************************)
(* THE WORLD                                                               *)
(***************************************************************************)
ExFill(o) ==
    /\ truth[o] = "open"
    /\ truth' = [truth EXCEPT ![o] = "final"]
    /\ UNCHANGED <<sgen, sconn, sdrops, admitted, released, b, ingress, reqq, evq, side,
                   tracked, answered, jepoch, jrunning, tick, rec_ok, tok_ok, issued>>

StreamConnect ==
    /\ ~sconn /\ sgen < MaxGen
    /\ sgen' = sgen + 1 /\ sconn' = TRUE /\ sdrops' = 0
    /\ UNCHANGED <<truth, admitted, released, b, ingress, reqq, evq, side,
                   tracked, answered, jepoch, jrunning, tick, rec_ok, tok_ok, issued>>

StreamDisconnect ==
    /\ sconn
    /\ sconn' = FALSE
    /\ UNCHANGED <<truth, sgen, sdrops, admitted, released, b, ingress, reqq, evq, side,
                   tracked, answered, jepoch, jrunning, tick, rec_ok, tok_ok, issued>>

StreamLoss ==
    /\ sconn /\ sdrops < MaxDrops
    /\ sdrops' = sdrops + 1
    /\ UNCHANGED <<truth, sgen, sconn, admitted, released, b, ingress, reqq, evq, side,
                   tracked, answered, jepoch, jrunning, tick, rec_ok, tok_ok, issued>>

(***************************************************************************)
(* THE HOT THREAD                                                          *)
(***************************************************************************)

\* An Ambiguous submit: the order exists at the exchange, holds a registry slot and is pushed to the ingress ring.
Admit(o) ==
    /\ o \notin admitted
    /\ Len(ingress) < Cardinality(ORDERS)
    /\ admitted' = admitted \cup {o}
    /\ truth'    = [truth EXCEPT ![o] = "open"]
    /\ ingress'  = Append(ingress, o)
    /\ UNCHANGED <<sgen, sconn, sdrops, released, b, reqq, evq, side,
                   tracked, answered, jepoch, jrunning, tick, rec_ok, tok_ok, issued>>

\* SweepPump::service(): refresh the barrier from the feed; if it is Sweeping with nothing outstanding, post the request.
Service ==
    LET x == ObserveB(b) IN
    /\ IF x.st = "Sweeping" /\ ~x.out /\ Len(reqq) < ReqCap /\ x.ep <= MaxEpoch
         THEN /\ b'    = [x EXCEPT !.out = TRUE, !.bd = admitted]
              /\ reqq' = Append(reqq, x.ep)
         ELSE /\ b' = x
              /\ UNCHANGED reqq
    /\ UNCHANGED <<truth, sgen, sconn, sdrops, admitted, released, ingress, evq, side,
                   tracked, answered, jepoch, jrunning, tick, rec_ok, tok_ok, issued>>

\* A request nobody answered in time is replaced by a new epoch (take_sweep_request()'s timeout path).
Timeout ==
    /\ b.st = "Sweeping" /\ b.out
    /\ b' = [b EXCEPT !.ep = @ + 1, !.out = FALSE, !.bd = {}]
    /\ UNCHANGED <<truth, sgen, sconn, sdrops, admitted, released, ingress, reqq, evq, side,
                   tracked, answered, jepoch, jrunning, tick, rec_ok, tok_ok, issued>>

\* SweepPump::on_done() for a marker of epoch ev.e: refresh the barrier, then ReconnectBarrier::on_sweep_report().
\* The account snapshot being fresh is nondeterministic. THE CHECK: at the acceptance that makes the barrier Reconciled,
\* every order admitted before the CURRENT epoch's request is published as resolved or was confirmed live by the sweep.
HandleMarker(ev) ==
    LET x        == ObserveB(b)
        accepted == x.st = "Sweeping" /\ x.out /\ (~EpochChecked \/ ev.e = x.ep)
    IN IF accepted
         THEN \E fresh \in BOOLEAN :
                IF fresh
                  THEN /\ b'      = [x EXCEPT !.st = "Reconciled", !.out = FALSE]
                       /\ rec_ok' = (rec_ok /\ x.bd \subseteq (released \cup ev.live))
                  ELSE /\ b'      = [x EXCEPT !.ep = @ + 1, !.out = FALSE, !.bd = {}]
                       /\ rec_ok' = rec_ok
         ELSE /\ b'      = x
              /\ rec_ok' = rec_ok

\* drain_reconcile_events(): pop the head of the event ring.
HotPop ==
    /\ Len(evq) > 0
    /\ LET ev == Head(evq) IN
         IF ev.k = "res"
           THEN /\ released' = released \cup {ev.o}
                /\ b'        = b
                /\ rec_ok'   = rec_ok
           ELSE /\ released' = released
                /\ HandleMarker(ev)
    /\ evq' = Tail(evq)
    /\ UNCHANGED <<truth, sgen, sconn, sdrops, admitted, ingress, reqq, side,
                   tracked, answered, jepoch, jrunning, tick, tok_ok, issued>>

\* Only without MarkerInBand: the markers travel on a channel that is not ordered against the resolutions.
HotPopSide ==
    /\ ~MarkerInBand /\ Len(side) > 0
    /\ HandleMarker(Head(side))
    /\ side' = Tail(side)
    /\ UNCHANGED <<truth, sgen, sconn, sdrops, admitted, released, ingress, reqq, evq,
                   tracked, answered, jepoch, jrunning, tick, tok_ok, issued>>

\* SubmitReadiness::authorize(): (observe, then) decide. The ghost records whether the token rested on the live stream.
Authorize ==
    LET x == IF ObserveFirst THEN ObserveB(b) ELSE b IN
    /\ x.st = "Reconciled"
    /\ b'      = x
    /\ tok_ok' = (tok_ok /\ x.gen = sgen /\ sconn)
    /\ issued' = IF issued < 2 THEN issued + 1 ELSE issued
    /\ UNCHANGED <<truth, sgen, sconn, sdrops, admitted, released, ingress, reqq, evq, side,
                   tracked, answered, jepoch, jrunning, tick, rec_ok>>

(***************************************************************************)
(* THE RECONCILE THREAD: one poll_once() call is  step 1, step 2, queries,  *)
(* completion. With TakeBeforeDrain step 1 takes the request and step 2     *)
(* drains the ingress ring; without it the order is the other way round.    *)
(***************************************************************************)
DoTake ==
    LET newest == IF reqq = << >> THEN 0 ELSE SeqMax(reqq) IN
    /\ IF newest > jepoch THEN /\ jepoch' = newest /\ jrunning' = TRUE
                          ELSE /\ jepoch' = jepoch /\ jrunning' = jrunning
    /\ reqq' = << >>

DoDrain ==
    /\ tracked' = tracked \cup SeqToSet(ingress)
    /\ ingress' = << >>

TickStep1 ==
    /\ tick = 0 /\ tick' = 1
    /\ IF TakeBeforeDrain
         THEN /\ DoTake /\ UNCHANGED <<tracked, ingress>>
         ELSE /\ DoDrain /\ UNCHANGED <<reqq, jepoch, jrunning>>
    /\ UNCHANGED <<truth, sgen, sconn, sdrops, admitted, released, b, evq, side,
                   answered, rec_ok, tok_ok, issued>>

TickStep2 ==
    /\ tick = 1 /\ tick' = 2
    /\ IF TakeBeforeDrain
         THEN /\ DoDrain /\ UNCHANGED <<reqq, jepoch, jrunning>>
         ELSE /\ DoTake /\ UNCHANGED <<tracked, ingress>>
    /\ UNCHANGED <<truth, sgen, sconn, sdrops, admitted, released, b, evq, side,
                   answered, rec_ok, tok_ok, issued>>

\* Ask the exchange about a tracked order. Final: the resolution is pushed and the order leaves the table. Still open: a definite
\* same-state answer, which counts for the sweep in progress.
Query(o) ==
    /\ tick = 2 /\ o \in tracked
    /\ IF truth[o] = "final"
         THEN /\ Len(evq) < EvCap
              /\ evq'      = Append(evq, [k |-> "res", o |-> o, e |-> 0, live |-> {}])
              /\ tracked'  = tracked \ {o}
              /\ answered' = answered
         ELSE /\ jrunning
              /\ answered' = [answered EXCEPT ![o] = jepoch]
              /\ evq'      = evq
              /\ tracked'  = tracked
    /\ UNCHANGED <<truth, sgen, sconn, sdrops, admitted, released, b, ingress, reqq, side,
                   jepoch, jrunning, tick, rec_ok, tok_ok, issued>>

\* No tracked order is waiting: the marker goes out AFTER every resolution pushed above, on the in-band ring or on the overtaking channel.
Complete ==
    /\ tick = 2 /\ jrunning
    /\ \A o \in tracked : answered[o] = jepoch
    /\ LET m == [k |-> "mark", o |-> "none", e |-> jepoch, live |-> tracked] IN
         IF MarkerInBand
           THEN /\ Len(evq) < EvCap /\ evq' = Append(evq, m) /\ side' = side
           ELSE /\ Len(side) < EvCap /\ side' = Append(side, m) /\ evq' = evq
    /\ jrunning' = FALSE /\ tick' = 0
    /\ UNCHANGED <<truth, sgen, sconn, sdrops, admitted, released, b, ingress, reqq,
                   tracked, answered, jepoch, rec_ok, tok_ok, issued>>

EndTick ==
    /\ tick = 2 /\ tick' = 0
    /\ UNCHANGED <<truth, sgen, sconn, sdrops, admitted, released, b, ingress, reqq, evq, side,
                   tracked, answered, jepoch, jrunning, rec_ok, tok_ok, issued>>

Next ==
    \/ \E o \in ORDERS : ExFill(o)
    \/ StreamConnect \/ StreamDisconnect \/ StreamLoss
    \/ \E o \in ORDERS : Admit(o)
    \/ Service \/ Timeout \/ HotPop \/ HotPopSide \/ Authorize
    \/ TickStep1 \/ TickStep2
    \/ \E o \in ORDERS : Query(o)
    \/ Complete \/ EndTick

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* SAFETY                                                                  *)
(***************************************************************************)
ReconciledIsSwept   == rec_ok
TokensOnlyWhenReady == tok_ok

\* Reachability probe (its config MUST fail): at least one token can be issued, so the two invariants above are not vacuous.
NoTokenIssued == issued = 0

(***************************************************************************)
(* State constraint for the safety configs: bounded epochs and rings.       *)
(***************************************************************************)
StateConstraint == b.ep <= MaxEpoch /\ Len(evq) <= EvCap /\ Len(side) <= EvCap

=============================================================================
