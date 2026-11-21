--------------------------- MODULE kill_switch ---------------------------
EXTENDS Naturals

CONSTANTS MaxRevision, MaxGeneration, Fault

ASSUME /\ MaxRevision >= 4
       /\ MaxGeneration >= 2
       /\ Fault \in {"None", "StaleCheck", "NoRevisionCheck", "EarlyReset", "RestoreNormal"}

VARIABLES runtime, disk, write, token, request

vars == <<runtime, disk, write, token, request>>
KillStates == {"Normal", "Restricted"}
Revisions == 0..MaxRevision
Generations == 0..MaxGeneration

NoWrite == [phase |-> "Idle", target |-> "Restricted"]
NoToken == [phase |-> "Idle", generation |-> 0, revision |-> 0]
NoRequest == [phase |-> "Idle", validAtClaim |-> TRUE, sentAfterRestriction |-> FALSE]

TypeOK ==
    /\ runtime \in [state: KillStates,
                    health: {"Stopped", "Ready", "Writing", "Degraded", "Corrupt"},
                    generation: Generations, revision: Revisions, resetAcknowledged: BOOLEAN]
    /\ disk \in [state: KillStates \cup {"Missing", "Unverified"}, revision: Revisions]
    /\ write \in [phase: {"Idle", "Pending", "Synced"}, target: KillStates]
    /\ token \in [phase: {"Idle", "Issued", "Checked"}, generation: Generations, revision: Revisions]
    /\ request \in [phase: {"Idle", "Claimed", "Sent"}, validAtClaim: BOOLEAN, sentAfterRestriction: BOOLEAN]

Init ==
    /\ runtime = [state |-> "Restricted", health |-> "Stopped",
                  generation |-> 0, revision |-> 0, resetAcknowledged |-> FALSE]
    /\ disk \in {[state |-> "Missing", revision |-> 0], [state |-> "Unverified", revision |-> 0]}
    /\ write = NoWrite
    /\ token = NoToken
    /\ request = NoRequest

Advance(revision) == IF revision < MaxRevision THEN revision + 1 ELSE revision

\* Recovery models the completed startup decision. An unverified record
\* remains restricted. Pending operations do not resume after a crash.
Recover ==
    /\ runtime.health = "Stopped"
    /\ runtime.generation < MaxGeneration
    /\ LET recovered ==
             CASE disk.state = "Missing" -> [state |-> "Restricted", revision |-> 0]
               [] disk.state = "Normal" /\ Fault # "RestoreNormal" ->
                    [state |-> "Restricted", revision |-> Advance(disk.revision)]
               [] OTHER -> disk
       IN /\ disk' = recovered
          /\ runtime' = [state |-> IF recovered.state = "Normal" THEN "Normal" ELSE "Restricted",
                         health |-> IF recovered.state = "Unverified" THEN "Corrupt" ELSE "Ready",
                         generation |-> runtime.generation + 1, revision |-> recovered.revision,
                         resetAcknowledged |-> FALSE]
    /\ UNCHANGED <<write, token, request>>

Crash ==
    /\ runtime.health # "Stopped"
    /\ runtime.generation < MaxGeneration
    /\ runtime' = [runtime EXCEPT !.state = "Restricted", !.health = "Stopped", !.resetAcknowledged = FALSE]
    /\ write' = NoWrite
    /\ request' = NoRequest
    /\ UNCHANGED <<disk, token>>

\* BeginWrite publishes the restrictive state and the new revision under the
\* state mutex. RequestReset represents a reset already approved by an operator.
BeginWrite(target) ==
    /\ runtime.health \in {"Ready", "Degraded"}
    /\ write.phase = "Idle"
    /\ LET revision == Advance(runtime.revision)
           nextState == IF revision = MaxRevision THEN "Restricted" ELSE target
           early == nextState = "Normal" /\ Fault = "EarlyReset"
       IN /\ runtime' = [runtime EXCEPT
                           !.state = IF early THEN "Normal"
                                     ELSE IF runtime.state = "Restricted" THEN "Restricted" ELSE nextState,
                           !.health = IF early THEN "Ready" ELSE "Writing",
                           !.revision = revision]
          /\ write' = [phase |-> "Pending", target |-> nextState]
    /\ UNCHANGED <<disk, token, request>>

Restrict == BeginWrite("Restricted")
RequestReset == BeginWrite("Normal")

NewRecord == [state |-> write.target, revision |-> runtime.revision]

SyncWrite ==
    /\ write.phase = "Pending"
    /\ disk' = NewRecord
    /\ write' = [write EXCEPT !.phase = "Synced"]
    /\ UNCHANGED <<runtime, token, request>>

PublishWrite ==
    /\ write.phase = "Synced"
    /\ runtime' = [runtime EXCEPT !.state = write.target, !.health = "Ready",
                                  !.resetAcknowledged = (write.target = "Normal")]
    /\ write' = NoWrite
    /\ UNCHANGED <<disk, token, request>>

\* A failed acknowledgement may leave the prior record, the replacement or
\* an unresolved temporary file. All three outcomes keep authorization closed.
FailWrite ==
    /\ write.phase = "Pending"
    /\ disk' \in {disk, NewRecord, [disk EXCEPT !.state = "Unverified"]}
    /\ runtime' = [runtime EXCEPT !.state = "Restricted", !.health = "Degraded",
                                  !.resetAcknowledged = FALSE]
    /\ write' = NoWrite
    /\ UNCHANGED <<token, request>>

CanOpen == runtime.health = "Ready" /\ runtime.state = "Normal"
MatchesGeneration == token.generation = runtime.generation
MatchesRevision == token.revision = runtime.revision

CanClaim ==
    /\ token.phase \in {"Issued", "Checked"}
    /\ CanOpen
    /\ MatchesGeneration
    /\ MatchesRevision

IssueToken ==
    /\ CanOpen
    /\ request.phase = "Idle"
    /\ token' = [phase |-> "Issued", generation |-> runtime.generation, revision |-> runtime.revision]
    /\ UNCHANGED <<runtime, disk, write, request>>

CheckToken ==
    /\ Fault = "StaleCheck"
    /\ token.phase = "Issued"
    /\ CanClaim
    /\ token' = [token EXCEPT !.phase = "Checked"]
    /\ UNCHANGED <<runtime, disk, write, request>>

Claim ==
    /\ request.phase = "Idle"
    /\ CASE Fault = "StaleCheck" -> token.phase = "Checked" /\ runtime.health # "Stopped"
         [] Fault = "NoRevisionCheck" -> token.phase = "Issued" /\ CanOpen /\ MatchesGeneration
         [] OTHER -> CanClaim
    /\ request' = [NoRequest EXCEPT !.phase = "Claimed", !.validAtClaim = CanClaim]
    /\ token' = NoToken
    /\ UNCHANGED <<runtime, disk, write>>

Send ==
    /\ request.phase = "Claimed"
    /\ request' = [request EXCEPT !.phase = "Sent", !.sentAfterRestriction = (runtime.state = "Restricted")]
    /\ UNCHANGED <<runtime, disk, write, token>>

FinishRequest ==
    /\ request.phase = "Sent"
    /\ request' = NoRequest
    /\ UNCHANGED <<runtime, disk, write, token>>

Next ==
    \/ Recover
    \/ Crash
    \/ Restrict
    \/ RequestReset
    \/ SyncWrite
    \/ PublishWrite
    \/ FailWrite
    \/ IssueToken
    \/ CheckToken
    \/ Claim
    \/ Send
    \/ FinishRequest

Spec == Init /\ [][Next]_vars

ClaimMatchesPublication == request.validAtClaim
ResetRequiresDurableWrite == CanOpen => (disk.state = "Normal" /\ disk.revision = runtime.revision)
RestartRequiresNewReset == CanOpen => runtime.resetAcknowledged

ClaimAfterResetIsUnreachable == request.phase # "Claimed"
SendAfterRestrictionIsUnreachable == ~request.sentAfterRestriction
=============================================================================
