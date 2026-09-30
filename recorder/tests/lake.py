"""Test helper: build a small lake (segments + manifest + ledger) from real fixture frames."""

from __future__ import annotations

import json
from pathlib import Path

from conftest import FIXTURES, load_frames

from hy_recorder.clock import FakeClock
from hy_recorder.envelope import encode_record
from hy_recorder.ledger import Ledger, make_event
from hy_recorder.manifest import Manifest
from hy_recorder.segment import SegmentWriter
from hy_recorder.state import StateFile

SPOT_DEPTH = "spot:btcusdt@depth@100ms"
USDM_DEPTH = "usdm:btcusdt@depth@100ms"
SPOT_TRADE = "spot:btcusdt@trade"
USDM_AGG = "usdm:btcusdt@aggTrade"
USDM_MARK = "usdm:btcusdt@markPrice@1s"
USDM_FORCE = "usdm:!forceOrder@arr"


class LakeBuilder:
    def __init__(self, root: Path, *, level: int = 3) -> None:
        self.root = Path(root)
        self.state = StateFile(self.root)
        self.state.load()
        self.run, _ = self.state.begin_run()
        self.manifest = Manifest(self.root)
        self.ledger = Ledger(self.root)
        self.clock = FakeClock()
        self.level = level
        self.seq = 0
        self.gens: dict[str, int] = {}
        self.writers: dict[tuple[str, str], SegmentWriter] = {}

    # -- primitives --------------------------------------------------------------
    def next_q(self) -> int:
        self.seq += 1
        return self.seq

    def event(self, kind: str, **fields) -> int:
        q = self.next_q()
        self.ledger.append(
            make_event(self.run, q, self.clock.wall_us(), self.clock.mono_us(), kind, **fields)
        )
        return q

    def _writer(self, venue: str, cls: str) -> SegmentWriter:
        key = (venue, cls)
        if key not in self.writers:
            self.writers[key] = SegmentWriter(
                self.root,
                venue,
                cls,
                level=self.level,
                next_segseq=self.state.next_segseq,
                manifest=self.manifest,
                clock=self.clock,
            )
        return self.writers[key]

    def open_conn(self, conn: str, streams: list[str]) -> int:
        gen = self.gens.get(conn, 0) + 1
        self.gens[conn] = gen
        self.event("WS_OPEN", conn=conn, gen=gen, streams=streams)
        return gen

    def record(
        self,
        venue: str,
        cls: str,
        stream: str,
        payload: bytes,
        gen: int,
        *,
        kind: str = "m",
        consume_q: bool = True,
        dt_us: int = 1000,
    ) -> int:
        q = self.next_q() if consume_q else self.seq
        wall = self.clock.wall_us()
        line, _ = encode_record(
            run=self.run,
            seq=q,
            gen=gen,
            wall_us=wall,
            mono_us=self.clock.mono_us(),
            stream=stream,
            payload=payload,
            kind=kind,
        )
        self._writer(venue, cls).write(line, run=self.run, seq=q, wall_us=wall, stream=stream)
        self.clock.advance(dt_us / 1e6)
        return q

    def stop(self, *, clean: bool = True) -> None:
        if clean:
            self.event("PROC_STOP", reason="test")
        self.ledger.close()
        for w in self.writers.values():
            w.seal("test")
        self.state.mark_clean() if clean else None

    def crash(self) -> None:
        """No PROC_STOP, writers abandoned with their .part files (block-flushed)."""
        self.ledger.close()
        for w in self.writers.values():
            w.flush_block()
            w.sync()
            w.abort_for_test()


# -- real-data scenarios ---------------------------------------------------------------------
def depth_frames(venue: str) -> list[bytes]:
    return [p for _, _, p in load_frames("%s_depth_bridge_frames.jsonl" % venue)]


def snapshot_body(venue: str) -> bytes:
    return (FIXTURES / ("%s_depth_snapshot_l100.json" % venue)).read_bytes().strip()


def snapshot_L(venue: str) -> int:
    return json.loads(snapshot_body(venue))["lastUpdateId"]


def write_depth(
    b: LakeBuilder,
    venue: str,
    gen: int,
    *,
    snapshot_after: int | None = 2,
    drop: set[int] | None = None,
    skip_snapshot: bool = False,
    hook=None,
) -> None:
    """Write the real depth frames of ``venue`` in generation ``gen`` with the snapshot after frame N."""
    stream = "%s:btcusdt@depth@100ms" % venue
    for i, payload in enumerate(depth_frames(venue)):
        if drop and i in drop:
            continue
        q = b.record(venue, "depth", stream, payload, gen)
        if hook is not None:
            hook(q)
        if snapshot_after is not None and i + 1 == snapshot_after and not skip_snapshot:
            b.record(venue, "snapshot", "%s:snapshot:BTCUSDT" % venue, snapshot_body(venue), gen, kind="s")


def write_simple(
    b: LakeBuilder,
    venue: str,
    cls: str,
    stream: str,
    fixture: str,
    gen: int,
    *,
    skip: set[int] | None = None,
    consume_q_on_skip: bool = False,
    limit: int | None = None,
) -> None:
    frames = [p for _, _, p in load_frames(fixture)]
    for i, payload in enumerate(frames[:limit]):
        if skip and i in skip:
            if consume_q_on_skip:
                b.next_q()
            continue
        b.record(venue, cls, stream, payload, gen)
