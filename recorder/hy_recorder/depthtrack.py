"""Online, lightweight bridge tracking for one depth stream.

It runs the same :class:`~hy_recorder.bridge.BridgeMachine` the offline verifier uses, but only to
drive scheduling and ledger events (snapshot needed, bridged, gap detected). It is advisory: the
offline verifier re-derives everything from the raw records and never trusts these events as proof.
"""

from __future__ import annotations

from collections.abc import Callable
from typing import Any

from .bridge import BridgeMachine, EventRef, Transition


class DepthTracker:
    def __init__(
        self,
        venue: str,
        symbol: str,
        stream_id: str,
        emit: Callable[..., None],
        *,
        on_first_event: Callable[[int], None],
        on_bridged: Callable[[int], None],
        on_gap: Callable[[str], None],
    ) -> None:
        self.venue = venue
        self.symbol = symbol
        self.stream_id = stream_id
        self.machine = BridgeMachine(venue)
        self.gen = 0
        self._emit = emit
        self._on_first_event = on_first_event
        self._on_bridged = on_bridged
        self._on_gap = on_gap

    @property
    def state(self) -> str:
        return self.machine.state.value

    def new_generation(self, gen: int) -> None:
        self.gen = gen
        self.machine.reset()

    def on_event(self, seq: int, U: int, u: int, pu: int | None, wall_us: int) -> list[Transition]:
        trs = self.machine.on_event(EventRef(seq, U, u, pu, wall_us))
        self._handle(trs)
        return trs

    def on_snapshot(self, gen: int, L: int, seq: int) -> list[Transition]:
        """Feed a snapshot for generation ``gen``; ignored if the generation has changed meanwhile."""
        if gen != self.gen:
            return []
        trs = self.machine.on_snapshot(L, seq)
        self._handle(trs)
        return trs

    def _handle(self, trs: list[Transition]) -> None:
        for tr in trs:
            d: dict[str, Any] = tr.detail
            if tr.kind == "BUFFERING":
                self._on_first_event(self.gen)
            elif tr.kind == "BRIDGED":
                self._emit(
                    "BRIDGE_OK",
                    stream=self.stream_id,
                    gen=self.gen,
                    bridge_q=tr.q,
                    L=d["L"],
                    U=d["U"],
                    u=d["u"],
                )
                self._on_bridged(self.gen)
            elif tr.kind == "GAP":
                self._emit(
                    "GAP_DETECTED",
                    stream=self.stream_id,
                    gen=self.gen,
                    bad_q=tr.q,
                    rule=d["rule"],
                    expected=d["expected"],
                    got=d["got"],
                )
                self._on_gap(d["rule"])
            elif tr.kind == "SNAPSHOT_STALE":
                self._emit(
                    "SNAPSHOT_STALE",
                    stream=self.stream_id,
                    gen=self.gen,
                    L=d.get("L"),
                    first_U=d.get("first_U"),
                )
