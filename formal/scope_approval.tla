---------------------------- MODULE scope_approval ----------------------------
(* R-25 design slice (docs/SCOPE_APPROVAL_DESIGN.md): authorization by a signed SCOPE instead of a per-order CONFIRM.
 *
 * WHAT THIS MODELS. The stateful part of the contract that would replace the operator's per-order confirmation
 * (OrderConfirmation, Gate 11 of live_submit_orchestrator.hpp) once it has passed its own acceptance: a grant the Owner
 * signs on his own machine (serial number, validity, budget), the executor that installs it, charges orders against it, and
 * consumes it at the final send gate (blueprint 5.9.2, task 1A.6, docs/NOTSENT_CLOSURE_DESIGN.md), and everything that can
 * happen to that grant meanwhile: revocation, replacement by a newer grant, expiry, a kill-switch latch, a crash, a clock
 * fault, the durable store being wiped. The claims it checks (the design config must satisfy all of them):
 *
 *   BudgetNeverExceeded        the number of orders that may have crossed under one grant never exceeds its budget --
 *                              across crashes, recovery, replayed grants and a wiped store.
 *   NoClaimAfterRevokeAck      once the Owner has been told "revoked", no new order is claimed under that grant.
 *   NoClaimUnderSuperseded     once a newer grant is installed, no new order is claimed under an older one (a newer grant
 *                              can only narrow what the Owner authorizes; narrowing must be effective).
 *   NoClaimUnderKilledScope    a grant that was live when the kill switch latched is dead for good: clearing the latch does
 *                              not bring it back (resuming automatic orders needs a new grant, i.e. the Owner's signature).
 *   NoClaimAfterExpiry         no order is claimed under a grant after its expiry instant (by TRUE time, not by what the
 *                              executor believes).
 *   NoClaimWhileKilled         the kill latch always refuses: a fresh grant never overrides it.
 *   ClosedImpliesNeverCrossed  a charge is given back only against a durable proof that the order never crossed.
 *   LiveGrantsAreSigned        only what the Owner signed is ever live.
 *   CrossedImpliesDurableCharge / MemoryCountsMatchOrders / HWMCoversStatuses   the structural reasons behind the above.
 *
 * WHAT IT IS NOT. A model of the PROTOCOL, not of the code: nothing here is implemented. It does not model the signature
 * itself (a grant is valid iff the Owner signed it: `signed` is the only source), the grant's static limits (symbol set,
 * per-order cap, price deviation, net exposure: pure functions of the order, unit-testable, no state), the frame layout, more
 * than one executor (host fencing, task 6.7), or the decision word / NotSent protocol itself (docs/NOTSENT_CLOSURE_DESIGN.md,
 * formal/notsent_closure.tla prove that one; here a refused order is simply closed in one step). Progress is not claimed: a
 * system that refuses everything satisfies every invariant here, which is why the reachability probes exist.
 *
 * CONVENTIONS (formal/notsent_closure.tla)
 *   Durable (survives Crash; only Wipe resets it): epoch dStatus dHWM dCharge dClosed dKill.
 *   Volatile (wiped by Crash, rebuilt by Recover): up mAct mRev mUsed mDeadline os oScope seen.
 *   Ghost (no process reads it): ackedInstall ackedRevoke killedArts artEpoch crossed crossedOrd bad -- the ground truth the
 *   invariants compare the executor's beliefs against.
 *   Every durable write in this model is ONE atomic step (it lands, or the process crashes before). The late-fsync
 *   subtleties of a Failed append are proved in notsent_closure.tla; here a charge that landed late can only make the budget
 *   tighter, never looser, so they are not repeated.
 *
 * THE TWELVE SWITCHES (TRUE = the design). A config that turns one off MUST fail, with the invariant named in its file; that
 * is what keeps the model from proving nothing.
 *
 *   AtomicScopeClaim        TRUE: "is the grant still live?" and "claim the order" are one step (the consumption point,
 *                           5.9.2). FALSE: the answer is read in an earlier step and the claim trusts it.
 *   ChargeBeforeClaim       TRUE: the order's charge against the grant rides on its durable Prepared frame, i.e. is durable
 *                           before the claim. FALSE: it is appended after the send.
 *   RebuildFromLog          TRUE: recovery recomputes every grant's used budget from the durable charges. FALSE: the
 *                           in-memory counters simply start from zero after a restart.
 *   ChargeHeldUntilResolved TRUE: an order that recovered as Ambiguous keeps its charge until it is closed WITH PROOF (a
 *                           durable NotSent, or the operator tool of D5 certifying that it never crossed). FALSE: a "leak
 *                           guard" gives the charge back after a while.
 *   ReplayGuard             TRUE: a grant installs only if its serial is strictly above the durable high-water mark (and a
 *                           revocation raises the mark to the revoked serial). FALSE: any validly signed grant installs.
 *   BindStore               TRUE: a grant is signed for one store identity (epoch); a store that was wiped has a new one and
 *                           refuses the old grants. FALSE: the grant does not name the store.
 *   LatestWins              TRUE: installing a grant atomically revokes every older live grant. FALSE: older grants stay
 *                           live next to the new one.
 *   RevokeAckAfterDurable   TRUE: the Owner is told "revoked" only after the revocation is durable. FALSE: as soon as the
 *                           executor has noted it in memory.
 *   KillInvalidatesScope    TRUE: the durable kill latch also revokes every live grant, atomically (one frame, one replay
 *                           rule). FALSE: the latch only blocks claims, and the grant is live again when it is cleared.
 *   AbsoluteExpiry          TRUE: expiry is an instant the Owner signed, re-evaluated after every restart. FALSE: the
 *                           executor turns it into "valid for N from install/boot" and re-arms that after a restart.
 *   RequireClockToken       TRUE: the expiry check fails closed unless the clock token is valid (a reading within Skew of
 *                           true time). FALSE: whatever the local clock says is used.
 *   PessimisticExpiry       TRUE: a grant counts as expired when reading + Skew passes its expiry (the clock may be Skew
 *                           behind). FALSE: the raw reading is compared.
 *
 * ENVIRONMENT ASSUMPTIONS (not switches, not modeled)
 *   A1 the signature scheme is sound: only the Owner's key produces a valid grant.
 *   A2 a truncated durable log is detected by the tip anchor and fails closed (inherited, durable_audit_sink.hpp). What is
 *      NOT detected is a restore of an OLDER, internally consistent copy of the whole store: that is a declared limit
 *      (docs/SCOPE_APPROVAL_DESIGN.md, section 9), with mitigations outside this model.
 *   A3 the durable kill latch exists and is not cleared by a restart (task SAFE-04); today's KillSwitch is in memory.
 *   A4 one executor at a time (task 6.7).
 *   A5 the D5 operator tool only certifies "never crossed" for an order that really never crossed (its own model proves it).
 *
 * KNOWN RESOLUTION LIMITS
 *   - Budget is counted in orders (units of one). The real unit is notional in integer quote-asset atoms; the protocol
 *     properties do not depend on the unit.
 *   - Time is a small integer; Skew and the readings model the clock token, not the clock.
 *   - An order that recovers without any durable charge is Idle again (a new attempt has a new client-order-id; the id is
 *     only a slot label here), so `crossed` counts per grant, never per id.
 *)
EXTENDS Naturals, FiniteSets, TLC

CONSTANTS
    AtomicScopeClaim,
    ChargeBeforeClaim,
    RebuildFromLog,
    ChargeHeldUntilResolved,
    ReplayGuard,
    BindStore,
    LatestWins,
    RevokeAckAfterDurable,
    KillInvalidatesScope,
    AbsoluteExpiry,
    RequireClockToken,
    PessimisticExpiry,
    MaxCrashes,       \* modeling budget, NOT a design bound
    MaxWipes,         \* modeling budget: how many times the durable store is replaced by an empty one
    MaxClockFaults,   \* modeling budget
    MaxKills          \* modeling budget

ASSUME AtomicScopeClaim       \in BOOLEAN
ASSUME ChargeBeforeClaim      \in BOOLEAN
ASSUME RebuildFromLog         \in BOOLEAN
ASSUME ChargeHeldUntilResolved \in BOOLEAN
ASSUME ReplayGuard            \in BOOLEAN
ASSUME BindStore              \in BOOLEAN
ASSUME LatestWins             \in BOOLEAN
ASSUME RevokeAckAfterDurable  \in BOOLEAN
ASSUME KillInvalidatesScope   \in BOOLEAN
ASSUME AbsoluteExpiry         \in BOOLEAN
ASSUME RequireClockToken      \in BOOLEAN
ASSUME PessimisticExpiry      \in BOOLEAN
ASSUME MaxCrashes \in Nat
ASSUME MaxWipes \in Nat
ASSUME MaxClockFaults \in Nat
ASSUME MaxKills \in Nat

Orders    == {1, 2}
MaxSerial == 2
Serials   == 1..MaxSerial
Epochs    == 0..MaxWipes
\* A signed artifact: <<serial, the store epoch it was signed for>>. The Owner reads the store's identity before he signs.
Arts    == Serials \X Epochs

MaxT == 3      \* true time runs 0..MaxT
Skew == 1      \* while the clock token is valid, a reading is within Skew of true time

Budget(g)   == 1
Expiry(g)   == IF g = 1 THEN 2 ELSE 3
Validity(g) == IF g = 1 THEN 2 ELSE 3   \* the duration a defective executor derives from the grant (AbsoluteExpiry = FALSE)

OrderStates   == {"Down", "Idle", "Admitted", "Prepared", "Claimed", "Sent", "Ambiguous", "Closed"}
ChargedStates == {"Admitted", "Prepared", "Claimed", "Sent", "Ambiguous"}   \* states whose charge the memory counters hold
Statuses      == {"None", "Active", "Revoked"}
BadKinds      == {"Expired", "RevokeAcked", "Superseded", "KilledScope", "Killed"}

VARIABLES
    t,              \* TRUE time, 0..MaxT
    clkOK,          \* BOOLEAN: the clock token is valid (the executor's readings are within Skew of t)
    signed,         \* SUBSET Arts: what the Owner has signed (the only source of valid grants)
    epoch,          \* durable: identity of the store (changes when the store is replaced by an empty one)
    dStatus,        \* durable: [Serials -> Statuses]
    dHWM,           \* durable: highest serial ever installed or revoked in this store
    dCharge,        \* durable: [Orders -> {0} u Serials]   the grant an order's Prepared frame charged (0 = none)
    dClosed,        \* durable: SUBSET Orders   orders closed with a NotSent proof (or, wrongly, by a leak guard)
    dKill,          \* durable: BOOLEAN   the kill latch
    up,             \* BOOLEAN: the executor process is running
    mAct,           \* volatile: SUBSET Serials   grants the executor holds as live
    mRev,           \* volatile: SUBSET Serials   grants noted as revoked, not necessarily durable yet
    mUsed,          \* volatile: [Serials -> Nat]   budget used, as the memory counts it
    mDeadline,      \* volatile: [Serials -> Nat]   a RELATIVE deadline, used only when AbsoluteExpiry = FALSE
    os,             \* volatile: [Orders -> OrderStates]
    oScope,         \* volatile: [Orders -> {0} u Serials]
    seen,           \* volatile: [Orders -> BOOLEAN]   a stale answer to "is the grant live?" (AtomicScopeClaim = FALSE only)
    crashesLeft, wipesLeft, faultsLeft, killsLeft,
    ackedInstall,   \* ghost: SUBSET (Serials \X Epochs)   installs the executor acknowledged (store epoch tagged)
    ackedRevoke,    \* ghost: SUBSET (Serials \X Epochs)   revocations the Owner was told about
    killedArts,     \* ghost: SUBSET (Serials \X Epochs)   grants that were live when a kill latched
    artEpoch,       \* ghost: [Serials -> Epochs]   which artifact (by the epoch it was signed for) sits in each serial slot
    crossed,        \* ghost: [Arts -> Nat]   orders that MAY have crossed under each artifact
    crossedOrd,     \* ghost: [Orders -> BOOLEAN]   did THIS attempt cross (claimed)?
    bad             \* ghost: SUBSET BadKinds   what was true of the world at some claim, and should not have been

envVars   == <<t, clkOK, signed, crashesLeft, wipesLeft, faultsLeft, killsLeft>>
durVars   == <<epoch, dStatus, dHWM, dCharge, dClosed, dKill>>
memVars   == <<up, mAct, mRev, mUsed, mDeadline, os, oScope, seen>>
ghostVars == <<ackedInstall, ackedRevoke, killedArts, artEpoch, crossed, crossedOrd, bad>>
vars      == <<envVars, durVars, memVars, ghostVars>>

Max2(a, b) == IF a >= b THEN a ELSE b

\* A counter that never goes below zero (the code's counters are unsigned and must not wrap, Engineering Mandate 5). Without
\* the floor a counter that FORGOT an order (RebuildFromLog = FALSE) would underflow on the refund and TypeOK would report the
\* symptom; with it the control reports the harm, which is the budget.
Dec(f, g) == [f EXCEPT ![g] = IF @ > 0 THEN @ - 1 ELSE 0]

TypeOK ==
    /\ t \in 0..MaxT
    /\ clkOK \in BOOLEAN
    /\ signed \subseteq Arts
    /\ epoch \in Epochs
    /\ dStatus \in [Serials -> Statuses]
    /\ dHWM \in 0..MaxSerial
    /\ dCharge \in [Orders -> {0} \cup Serials]
    /\ dClosed \subseteq Orders
    /\ dKill \in BOOLEAN
    /\ up \in BOOLEAN
    /\ mAct \subseteq Serials
    /\ mRev \subseteq Serials
    /\ mUsed \in [Serials -> Nat]
    /\ mDeadline \in [Serials -> Nat]
    /\ os \in [Orders -> OrderStates]
    /\ oScope \in [Orders -> {0} \cup Serials]
    /\ seen \in [Orders -> BOOLEAN]
    /\ crashesLeft \in 0..MaxCrashes
    /\ wipesLeft \in 0..MaxWipes
    /\ faultsLeft \in 0..MaxClockFaults
    /\ killsLeft \in 0..MaxKills
    /\ ackedInstall \subseteq Arts
    /\ ackedRevoke \subseteq Arts
    /\ killedArts \subseteq Arts
    /\ artEpoch \in [Serials -> Epochs]
    /\ crossed \in [Arts -> Nat]
    /\ crossedOrd \in [Orders -> BOOLEAN]
    /\ bad \subseteq BadKinds

Init ==
    /\ t = 0
    /\ clkOK = TRUE
    /\ signed = {}
    /\ epoch = 0
    /\ dStatus = [g \in Serials |-> "None"]
    /\ dHWM = 0
    /\ dCharge = [o \in Orders |-> 0]
    /\ dClosed = {}
    /\ dKill = FALSE
    /\ up = TRUE
    /\ mAct = {}
    /\ mRev = {}
    /\ mUsed = [g \in Serials |-> 0]
    /\ mDeadline = [g \in Serials |-> 0]
    /\ os = [o \in Orders |-> "Idle"]
    /\ oScope = [o \in Orders |-> 0]
    /\ seen = [o \in Orders |-> FALSE]
    /\ crashesLeft = MaxCrashes
    /\ wipesLeft = MaxWipes
    /\ faultsLeft = MaxClockFaults
    /\ killsLeft = MaxKills
    /\ ackedInstall = {}
    /\ ackedRevoke = {}
    /\ killedArts = {}
    /\ artEpoch = [g \in Serials |-> 0]
    /\ crossed = [a \in Arts |-> 0]
    /\ crossedOrd = [o \in Orders |-> FALSE]
    /\ bad = {}

(***************************************************************************)
(* The clock and the expiry check.                                          *)
(***************************************************************************)

\* What the executor's clock can show right now: within Skew of true time while the token is valid, anything otherwise.
Readings == IF clkOK
            THEN {x \in 0..(MaxT + Skew) : x + Skew >= t /\ x <= t + Skew}
            ELSE 0..(MaxT + Skew)

Margin == IF PessimisticExpiry THEN Skew ELSE 0

\* The limit a grant is checked against, in the clock's own units: the instant the Owner signed, or (the defect) a deadline
\* the executor derived from a duration.
Limit(g) == IF AbsoluteExpiry THEN Expiry(g) ELSE mDeadline[g]

ExpiryOK(g, r) == /\ (RequireClockToken => clkOK)
                  /\ r + Margin <= Limit(g)

\* The executor's whole belief about a grant at the consumption point.
Live(g, r) == /\ g \in mAct
              /\ g \notin mRev
              /\ ExpiryOK(g, r)

\* What is TRUE, by ground truth, at the instant an order is claimed under g. A flag here is a defect of the design.
TruthFlags(g) ==
    {k \in BadKinds :
        \/ k = "Expired"     /\ t > Expiry(g)
        \/ k = "RevokeAcked" /\ <<g, artEpoch[g]>> \in ackedRevoke
        \/ k = "Superseded"  /\ \E a \in ackedInstall : a[2] = epoch /\ a[1] > g
        \/ k = "KilledScope" /\ <<g, artEpoch[g]>> \in killedArts
        \/ k = "Killed"      /\ dKill}

(***************************************************************************)
(* The Owner, the clock, the environment.                                   *)
(***************************************************************************)

\* The Owner signs a grant for the store he sees (its epoch), in serial order. The artifact then exists for ever: anyone who
\* copied it can present it again, at any time, to any store -- that is the replay the design has to survive.
OwnerSign(a) ==
    /\ a \in Arts
    /\ a \notin signed
    /\ a[2] = epoch
    /\ (a[1] = 1 \/ <<a[1] - 1, epoch>> \in signed)
    /\ signed' = signed \cup {a}
    /\ UNCHANGED <<t, clkOK, crashesLeft, wipesLeft, faultsLeft, killsLeft, durVars, memVars, ghostVars>>

Tick ==
    /\ t < MaxT
    /\ t' = t + 1
    /\ UNCHANGED <<clkOK, signed, crashesLeft, wipesLeft, faultsLeft, killsLeft, durVars, memVars, ghostVars>>

ClockFault ==
    /\ clkOK
    /\ faultsLeft > 0
    /\ faultsLeft' = faultsLeft - 1
    /\ clkOK' = FALSE
    /\ UNCHANGED <<t, signed, crashesLeft, wipesLeft, killsLeft, durVars, memVars, ghostVars>>

ClockResync ==
    /\ ~clkOK
    /\ clkOK' = TRUE
    /\ UNCHANGED <<t, signed, crashesLeft, wipesLeft, faultsLeft, killsLeft, durVars, memVars, ghostVars>>

(***************************************************************************)
(* Installing and revoking grants.                                          *)
(***************************************************************************)

\* The executor receives a signed grant (any artifact the Owner ever signed). One atomic durable write: the status of the
\* serial, the revocation of every older live grant (LatestWins) and the high-water mark.
Install(a) ==
    /\ up
    /\ a \in signed
    /\ a[1] \notin mRev
    /\ (BindStore => a[2] = epoch)
    /\ (ReplayGuard => a[1] > dHWM)
    /\ \E r \in (IF AbsoluteExpiry THEN {0} ELSE Readings) :
         /\ dStatus' = [k \in Serials |->
                           IF k = a[1] THEN "Active"
                           ELSE IF LatestWins /\ k < a[1] /\ dStatus[k] = "Active" THEN "Revoked"
                           ELSE dStatus[k]]
         /\ mAct' = {k \in Serials : dStatus'[k] = "Active"}
         /\ mDeadline' = IF AbsoluteExpiry THEN mDeadline ELSE [mDeadline EXCEPT ![a[1]] = r + Validity(a[1])]
    /\ dHWM' = Max2(dHWM, a[1])
    /\ ackedInstall' = ackedInstall \cup {<<a[1], epoch>>}
    /\ artEpoch' = [artEpoch EXCEPT ![a[1]] = a[2]]
    /\ UNCHANGED <<envVars, epoch, dCharge, dClosed, dKill, up, mRev, mUsed, os, oScope, seen,
                   ackedRevoke, killedArts, crossed, crossedOrd, bad>>

\* The revocation reaches the executor (a local command: tightening needs no signature). Memory first, so claims stop at once.
RevokeMem(g) ==
    /\ up
    /\ <<g, epoch>> \in signed
    /\ g \notin mRev
    /\ mRev' = mRev \cup {g}
    /\ ackedRevoke' = IF RevokeAckAfterDurable THEN ackedRevoke ELSE ackedRevoke \cup {<<g, epoch>>}
    /\ UNCHANGED <<envVars, durVars, up, mAct, mUsed, mDeadline, os, oScope, seen,
                   ackedInstall, killedArts, artEpoch, crossed, crossedOrd, bad>>

\* ... then durable, and only then the Owner is told. The mark also covers a serial that was never installed.
RevokeDurable(g) ==
    /\ up
    /\ g \in mRev
    /\ ~(dStatus[g] = "Revoked" /\ g <= dHWM)
    /\ dStatus' = [dStatus EXCEPT ![g] = "Revoked"]
    /\ dHWM' = Max2(dHWM, g)
    /\ mAct' = mAct \ {g}
    /\ ackedRevoke' = ackedRevoke \cup {<<g, epoch>>}
    /\ UNCHANGED <<envVars, epoch, dCharge, dClosed, dKill, up, mRev, mUsed, mDeadline, os, oScope, seen,
                   ackedInstall, killedArts, artEpoch, crossed, crossedOrd, bad>>

\* The kill latch becomes durable. With KillInvalidatesScope the same atomic write revokes every live grant.
Kill ==
    /\ up
    /\ killsLeft > 0
    /\ ~dKill
    /\ killsLeft' = killsLeft - 1
    /\ dKill' = TRUE
    /\ IF KillInvalidatesScope
       THEN /\ dStatus' = [k \in Serials |-> IF dStatus[k] = "Active" THEN "Revoked" ELSE dStatus[k]]
            /\ mAct' = {}
       ELSE /\ dStatus' = dStatus
            /\ mAct' = mAct
    /\ killedArts' = killedArts \cup {<<g, artEpoch[g]>> : g \in mAct}
    /\ UNCHANGED <<t, clkOK, signed, crashesLeft, wipesLeft, faultsLeft, epoch, dHWM, dCharge, dClosed,
                   up, mRev, mUsed, mDeadline, os, oScope, seen,
                   ackedInstall, ackedRevoke, artEpoch, crossed, crossedOrd, bad>>

\* Only an explicit operator action clears the latch (ADR-015; the latch is durable, task SAFE-04).
OperatorClearKill ==
    /\ up
    /\ dKill
    /\ dKill' = FALSE
    /\ UNCHANGED <<envVars, epoch, dStatus, dHWM, dCharge, dClosed, memVars, ghostVars>>

(***************************************************************************)
(* One order's life: admission, the Prepared frame, the final gate.        *)
(***************************************************************************)

\* Gate 11's replacement: the grant is live and has budget left. Memory only: a reservation, no durable trace yet.
Admit(o) ==
    /\ up
    /\ os[o] = "Idle"
    /\ \E g \in mAct :
         /\ g \notin mRev
         /\ mUsed[g] < Budget(g)
         /\ oScope' = [oScope EXCEPT ![o] = g]
         /\ mUsed' = [mUsed EXCEPT ![g] = @ + 1]
    /\ os' = [os EXCEPT ![o] = "Admitted"]
    /\ crossedOrd' = [crossedOrd EXCEPT ![o] = FALSE]
    /\ UNCHANGED <<envVars, durVars, up, mAct, mRev, mDeadline, seen,
                   ackedInstall, ackedRevoke, killedArts, artEpoch, crossed, bad>>

\* A refusal before the Prepared frame: nothing durable exists, the reservation just goes.
AbandonAdmitted(o) ==
    /\ up
    /\ os[o] = "Admitted"
    /\ os' = [os EXCEPT ![o] = "Idle"]
    /\ mUsed' = Dec(mUsed, oScope[o])
    /\ oScope' = [oScope EXCEPT ![o] = 0]
    /\ UNCHANGED <<envVars, durVars, up, mAct, mRev, mDeadline, seen, ghostVars>>

\* Gate 12d: the durable Prepared frame. With ChargeBeforeClaim it carries the charge (which grant), so the charge is durable
\* before any claim and costs no extra append.
Prepare(o) ==
    /\ up
    /\ os[o] = "Admitted"
    /\ os' = [os EXCEPT ![o] = "Prepared"]
    /\ dCharge' = IF ChargeBeforeClaim THEN [dCharge EXCEPT ![o] = oScope[o]] ELSE dCharge
    /\ seen' = [seen EXCEPT ![o] = FALSE]
    /\ UNCHANGED <<envVars, epoch, dStatus, dHWM, dClosed, dKill, up, mAct, mRev, mUsed, mDeadline, oScope, ghostVars>>

\* The effect of a claim, shared by the atomic and the stale variant.
ClaimEffect(o) ==
    LET g == oScope[o] IN
    /\ os' = [os EXCEPT ![o] = "Claimed"]
    /\ crossed' = [crossed EXCEPT ![<<g, artEpoch[g]>>] = @ + 1]
    /\ crossedOrd' = [crossedOrd EXCEPT ![o] = TRUE]
    /\ bad' = bad \cup TruthFlags(g)

\* The final gate (5.9.2): the liveness test and the claim are one step. A latched kill switch always refuses (gate 4 of the seven).
Claim(o) ==
    /\ AtomicScopeClaim
    /\ up
    /\ os[o] = "Prepared"
    /\ \E r \in Readings : Live(oScope[o], r)
    /\ ~dKill
    /\ ClaimEffect(o)
    /\ UNCHANGED <<envVars, durVars, up, mAct, mRev, mUsed, mDeadline, oScope, seen, ackedInstall, ackedRevoke, killedArts, artEpoch>>

\* AtomicScopeClaim = FALSE: the test is a step of its own ...
ObserveScope(o) ==
    /\ ~AtomicScopeClaim
    /\ up
    /\ os[o] = "Prepared"
    /\ \E r \in Readings : seen' = [seen EXCEPT ![o] = Live(oScope[o], r)]
    /\ UNCHANGED <<envVars, durVars, up, mAct, mRev, mUsed, mDeadline, os, oScope, ghostVars>>

\* ... and the claim trusts it.
ClaimStale(o) ==
    /\ ~AtomicScopeClaim
    /\ up
    /\ os[o] = "Prepared"
    /\ seen[o]
    /\ ~dKill
    /\ ClaimEffect(o)
    /\ UNCHANGED <<envVars, durVars, up, mAct, mRev, mUsed, mDeadline, oScope, seen, ackedInstall, ackedRevoke, killedArts, artEpoch>>

\* The final gate refused (the grant is not live, or the kill latch is set): the order is closed with a durable NotSent -- it
\* never crossed -- and its charge goes back. (The decision word is notsent_closure.tla's business.)
RejectAndClose(o) ==
    /\ up
    /\ os[o] = "Prepared"
    /\ \E r \in Readings : ~(Live(oScope[o], r) /\ ~dKill)
    /\ os' = [os EXCEPT ![o] = "Closed"]
    /\ dClosed' = dClosed \cup {o}
    /\ mUsed' = Dec(mUsed, oScope[o])
    /\ UNCHANGED <<envVars, epoch, dStatus, dHWM, dCharge, dKill, up, mAct, mRev, mDeadline, oScope, seen, ghostVars>>

Send(o) ==
    /\ up
    /\ os[o] = "Claimed"
    /\ os' = [os EXCEPT ![o] = "Sent"]
    /\ UNCHANGED <<envVars, durVars, up, mAct, mRev, mUsed, mDeadline, oScope, seen, ghostVars>>

\* ChargeBeforeClaim = FALSE: the charge is appended after the send.
ChargeAfterSend(o) ==
    /\ ~ChargeBeforeClaim
    /\ up
    /\ os[o] = "Sent"
    /\ dCharge[o] = 0
    /\ dCharge' = [dCharge EXCEPT ![o] = oScope[o]]
    /\ UNCHANGED <<envVars, epoch, dStatus, dHWM, dClosed, dKill, memVars, ghostVars>>

\* The way out of Ambiguous: the D5 operator tool certified that the order never crossed (assumption A5).
OperatorCloseAbsent(o) ==
    /\ up
    /\ os[o] = "Ambiguous"
    /\ ~crossedOrd[o]
    /\ os' = [os EXCEPT ![o] = "Closed"]
    /\ dClosed' = dClosed \cup {o}
    /\ mUsed' = Dec(mUsed, oScope[o])
    /\ UNCHANGED <<envVars, epoch, dStatus, dHWM, dCharge, dKill, up, mAct, mRev, mDeadline, oScope, seen, ghostVars>>

\* ChargeHeldUntilResolved = FALSE: a leak guard gives an Ambiguous order's charge back with no proof.
LeakGuardRelease(o) ==
    /\ ~ChargeHeldUntilResolved
    /\ up
    /\ os[o] = "Ambiguous"
    /\ os' = [os EXCEPT ![o] = "Closed"]
    /\ dClosed' = dClosed \cup {o}
    /\ mUsed' = Dec(mUsed, oScope[o])
    /\ UNCHANGED <<envVars, epoch, dStatus, dHWM, dCharge, dKill, up, mAct, mRev, mDeadline, oScope, seen, ghostVars>>

(***************************************************************************)
(* Crash, recovery, a wiped store.                                          *)
(***************************************************************************)

\* Volatile memory dies. The durable state does not.
Crash ==
    /\ up
    /\ crashesLeft > 0
    /\ crashesLeft' = crashesLeft - 1
    /\ up' = FALSE
    /\ mAct' = {}
    /\ mRev' = {}
    /\ mUsed' = [g \in Serials |-> 0]
    /\ mDeadline' = [g \in Serials |-> 0]
    /\ os' = [o \in Orders |-> "Down"]
    /\ oScope' = [o \in Orders |-> 0]
    /\ seen' = [o \in Orders |-> FALSE]
    /\ UNCHANGED <<t, clkOK, signed, wipesLeft, faultsLeft, killsLeft, durVars, ghostVars>>

\* Recovery rebuilds memory from the durable state alone: the live grants from the statuses, the used budget from the charges
\* (an order with a charge and no closing frame is Ambiguous and stays charged), the orders from the charges and closings.
Recover ==
    /\ ~up
    /\ up' = TRUE
    /\ mAct' = {g \in Serials : dStatus[g] = "Active"}
    /\ mRev' = {}
    /\ mUsed' = [g \in Serials |->
                    IF RebuildFromLog
                    THEN Cardinality({o \in Orders : dCharge[o] = g /\ o \notin dClosed})
                    ELSE 0]
    /\ os' = [o \in Orders |-> IF o \in dClosed THEN "Closed"
                               ELSE IF dCharge[o] # 0 THEN "Ambiguous"
                               ELSE "Idle"]
    /\ oScope' = [o \in Orders |-> dCharge[o]]
    /\ seen' = [o \in Orders |-> FALSE]
    /\ \E r \in (IF AbsoluteExpiry THEN {0} ELSE Readings) :
         mDeadline' = IF AbsoluteExpiry THEN mDeadline ELSE [g \in Serials |-> r + Validity(g)]
    /\ UNCHANGED <<envVars, durVars, ghostVars>>

\* The durable store is replaced by an empty one while the process is down (a new disk, a deleted directory, a fresh VM).
Wipe ==
    /\ ~up
    /\ wipesLeft > 0
    /\ wipesLeft' = wipesLeft - 1
    /\ epoch' = epoch + 1
    /\ dStatus' = [g \in Serials |-> "None"]
    /\ dHWM' = 0
    /\ dCharge' = [o \in Orders |-> 0]
    /\ dClosed' = {}
    /\ dKill' = FALSE
    /\ UNCHANGED <<t, clkOK, signed, crashesLeft, faultsLeft, killsLeft, memVars, ghostVars>>

Next ==
    \/ \E a \in Arts : OwnerSign(a)
    \/ Tick
    \/ ClockFault
    \/ ClockResync
    \/ \E a \in Arts : Install(a)
    \/ \E g \in Serials : RevokeMem(g)
    \/ \E g \in Serials : RevokeDurable(g)
    \/ Kill
    \/ OperatorClearKill
    \/ \E o \in Orders : Admit(o)
    \/ \E o \in Orders : AbandonAdmitted(o)
    \/ \E o \in Orders : Prepare(o)
    \/ \E o \in Orders : Claim(o)
    \/ \E o \in Orders : ObserveScope(o)
    \/ \E o \in Orders : ClaimStale(o)
    \/ \E o \in Orders : RejectAndClose(o)
    \/ \E o \in Orders : Send(o)
    \/ \E o \in Orders : ChargeAfterSend(o)
    \/ \E o \in Orders : OperatorCloseAbsent(o)
    \/ \E o \in Orders : LeakGuardRelease(o)
    \/ Crash
    \/ Recover
    \/ Wipe

Spec == Init /\ [][Next]_vars

\* crossed stays <= 2, NOT <= 1: a bound at the budget would make BudgetNeverExceeded unfalsifiable by construction.
StateConstraint ==
    /\ \A a \in Arts : crossed[a] <= 2
    /\ \A g \in Serials : mUsed[g] <= 2

(***************************************************************************)
(* Safety invariants -- a violation here means the DESIGN is wrong.         *)
(***************************************************************************)

\* The headline: the orders that MAY have crossed under one grant never exceed what the Owner signed.
BudgetNeverExceeded == \A a \in Arts : crossed[a] <= Budget(a[1])

\* Once the Owner was told "revoked", nothing new is claimed under that grant.
NoClaimAfterRevokeAck == "RevokeAcked" \notin bad

\* A newer installed grant ends the older ones: narrowing is effective.
NoClaimUnderSuperseded == "Superseded" \notin bad

\* A grant that was live when the kill latch set is dead for good.
NoClaimUnderKilledScope == "KilledScope" \notin bad

\* By TRUE time, not by the executor's belief.
NoClaimAfterExpiry == "Expired" \notin bad

\* The kill latch always refuses (gate 4 of the seven of 5.9.2): a grant, however fresh, never overrides it.
NoClaimWhileKilled == "Killed" \notin bad

\* Only what the Owner signed can be live (assumption A1 in use: the executor installs nothing else).
LiveGrantsAreSigned == \A g \in mAct : <<g, artEpoch[g]>> \in signed

\* The structural reason behind the budget: an order that may be on the wire always has its charge durable.
CrossedImpliesDurableCharge == \A o \in Orders : os[o] \in {"Claimed", "Sent"} => dCharge[o] # 0

\* A charge goes back only against proof: a closed order never crossed.
ClosedImpliesNeverCrossed == \A o \in Orders : o \in dClosed => ~crossedOrd[o]

\* After any recovery the memory counters are exactly what the orders' states say.
MemoryCountsMatchOrders ==
    up => \A g \in Serials :
              mUsed[g] = Cardinality({o \in Orders : os[o] \in ChargedStates /\ oScope[o] = g})

\* The mark covers every serial that has any durable status.
HWMCoversStatuses == \A g \in Serials : dStatus[g] # "None" => g <= dHWM

(***************************************************************************)
(* Reachability probes -- NOT invariants of anything. Each runs in its own  *)
(* cfg and TLC MUST report a violation: it shows the scenario the main      *)
(* config's "no error" depends on is actually explored.                     *)
(***************************************************************************)

\* An order can be sent under a live grant at all.
SentNeverHappens == \A o \in Orders : os[o] # "Sent"

\* ... under the SECOND grant too (the Owner narrowed or renewed, and trading went on).
SecondGrantSentNeverHappens == \A o \in Orders : ~(os[o] = "Sent" /\ oScope[o] = 2)

\* An order can be on the wire while its grant has been revoked and the Owner told: claim first, revoke second. The design
\* promises that a revocation which came first wins, not that bytes already sent are recalled.
SentThenRevokedNeverHappens ==
    \A o \in Orders : ~(os[o] = "Sent" /\ oScope[o] # 0 /\ <<oScope[o], artEpoch[oScope[o]]>> \in ackedRevoke)

\* A charge can be given back (the refunds the design allows are reachable).
NoRefundNeverHappens == \A o \in Orders : os[o] # "Closed"

\* An order can recover as Ambiguous and hold its charge.
NoAmbiguousNeverHappens == \A o \in Orders : os[o] # "Ambiguous"

====
