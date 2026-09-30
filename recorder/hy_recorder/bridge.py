"""Depth-book bridge state machine, one instance per (venue, symbol, connection generation).

States: ``UNVERIFIED -> BUFFERING -> BRIDGING -> VERIFIED -> GAP`` (GAP is terminal for the
generation: a gap has no common baseline on both sides, so a later snapshot must never be used
to declare the interval continuous - reconnect, fetch a snapshot and bridge again instead).

The rules are the official ones and differ per venue (DATA_DICTIONARY.md, protocol ledger
P-22 / P-23), verified against 5-minute real samples on 2026-09-29:

* spot  : discard events with ``u <= lastUpdateId``; the first kept event must satisfy
          ``U <= lastUpdateId + 1 <= u``; afterwards ``U`` must equal the previous ``u + 1``
          (``U`` greater than previous ``u + 1`` means missed events).
* usdm  : discard events with ``u < lastUpdateId``; the first kept event must satisfy
          ``U <= lastUpdateId`` and ``u >= lastUpdateId``; afterwards ``pu`` must equal the
          previous ``u``.

A snapshot whose ``lastUpdateId`` is strictly smaller than the ``U`` of the first buffered event
is stale (nothing in the buffer can bridge it) and must be re-fetched.

The same machine drives the recorder's lightweight online tracking (to schedule snapshots and
write ledger events) and the offline verifier (which re-derives everything from raw records and
does not trust the online ledger).
"""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass, field
from enum import Enum
from typing import Any

SPOT = "spot"
USDM = "usdm"


class BridgeState(str, Enum):
    UNVERIFIED = "UNVERIFIED"
    BUFFERING = "BUFFERING"
    BRIDGING = "BRIDGING"
    VERIFIED = "VERIFIED"
    GAP = "GAP"


@dataclass(frozen=True)
class EventRef:
    q: int
    U: int
    u: int
    pu: int | None = None
    t: int = 0  # host wall clock of the record (microseconds), carried through for interval bookkeeping


@dataclass(frozen=True)
class Transition:
    kind: str  # BUFFERING SNAPSHOT_STALE BRIDGED GAP OVERLAP KEYFRAME
    q: int
    detail: dict[str, Any] = field(default_factory=dict)


class BridgeMachine:
    def __init__(self, venue: str, *, buffer_cap: int = 4096, ring_size: int = 512) -> None:
        if venue not in (SPOT, USDM):
            raise ValueError("venue must be 'spot' or 'usdm'")
        self.venue = venue
        self._buffer_cap = buffer_cap
        self._ring: deque[EventRef] = deque(maxlen=ring_size)
        self.reset()

    # -- lifecycle ---------------------------------------------------------------
    def reset(self) -> None:
        """Start a new connection generation."""
        self.state = BridgeState.UNVERIFIED
        self.first_U: int | None = None
        self.snap_L: int | None = None
        self.prev_u: int | None = None
        self.bridge: dict[str, int] | None = None
        self._buf: list[EventRef] = []
        self._ring.clear()

    # -- rules -------------------------------------------------------------------
    def _is_candidate(self, ev: EventRef, L: int) -> bool:
        return ev.u > L if self.venue == SPOT else ev.u >= L

    def _bridge_ok(self, ev: EventRef, L: int) -> bool:
        return ev.U <= L + 1 if self.venue == SPOT else ev.U <= L

    # -- input -------------------------------------------------------------------
    def on_event(self, ev: EventRef) -> list[Transition]:
        if self.state is BridgeState.GAP:
            return []
        out: list[Transition] = []
        if self.state is BridgeState.UNVERIFIED:
            self.first_U = ev.U
            self.state = BridgeState.BUFFERING
            out.append(Transition("BUFFERING", ev.q, {"first_U": ev.U}))
        if self.state in (BridgeState.BUFFERING, BridgeState.BRIDGING):
            if len(self._buf) >= self._buffer_cap:
                del self._buf[0]
            self._buf.append(ev)
            self._ring.append(ev)
            if self.snap_L is not None:
                out.extend(self._try_bridge(ev.q))
            return out
        self._ring.append(ev)  # VERIFIED
        return self._chain(ev)

    def on_snapshot(self, L: int, q: int) -> list[Transition]:
        if self.state is BridgeState.GAP:
            return []
        if self.state is BridgeState.VERIFIED:
            covered = L == self.prev_u or any(
                (e.U <= L + 1 <= e.u) if self.venue == SPOT else (e.U <= L <= e.u) for e in self._ring
            )
            return [Transition("KEYFRAME", q, {"L": L, "covered": covered})]
        self.snap_L = L
        if self.state is BridgeState.UNVERIFIED:
            return []  # the first event will trigger the bridging attempt
        if self.first_U is not None and L < self.first_U:
            self.snap_L = None
            return [Transition("SNAPSHOT_STALE", q, {"L": L, "first_U": self.first_U})]
        return self._try_bridge(q)

    # -- internals ---------------------------------------------------------------
    def _try_bridge(self, q: int) -> list[Transition]:
        L = self.snap_L
        assert L is not None
        idx = next((i for i, e in enumerate(self._buf) if self._is_candidate(e, L)), None)
        if idx is None:
            self.state = BridgeState.BRIDGING  # snapshot is ahead of everything buffered so far
            return []
        cand = self._buf[idx]
        if not self._bridge_ok(cand, L):
            self.snap_L = None
            self.state = BridgeState.BUFFERING
            return [Transition("SNAPSHOT_STALE", q, {"L": L, "first_U": cand.U, "candidate_q": cand.q})]
        self.state = BridgeState.VERIFIED
        self.prev_u = cand.u
        self.bridge = {"L": L, "q": cand.q, "U": cand.U, "u": cand.u}
        out = [Transition("BRIDGED", cand.q, {"L": L, "U": cand.U, "u": cand.u, "pu": cand.pu, "t": cand.t})]
        later = self._buf[idx + 1 :]
        self._buf = []
        for ev in later:
            out.extend(self._chain(ev))
            if self.state is BridgeState.GAP:
                break
        return out

    def _chain(self, ev: EventRef) -> list[Transition]:
        assert self.prev_u is not None
        if self.venue == SPOT:
            if ev.U > self.prev_u + 1:
                self.state = BridgeState.GAP
                return [
                    Transition(
                        "GAP",
                        ev.q,
                        {"rule": "spot_U_gt_prev_u_plus_1", "expected": self.prev_u + 1, "got": ev.U},
                    )
                ]
            prev = self.prev_u
            if ev.u <= prev:  # duplicate or older event: nothing new, ignore it
                return [Transition("OVERLAP", ev.q, {"prev_u": prev, "U": ev.U, "u": ev.u})]
            self.prev_u = ev.u
            if ev.U <= prev:  # overlaps the applied range but carries newer updates: apply it
                return [Transition("OVERLAP", ev.q, {"prev_u": prev, "U": ev.U, "u": ev.u})]
            return []
        if ev.pu != self.prev_u:
            self.state = BridgeState.GAP
            return [
                Transition("GAP", ev.q, {"rule": "usdm_pu_ne_prev_u", "expected": self.prev_u, "got": ev.pu})
            ]
        self.prev_u = ev.u
        return []
