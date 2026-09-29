"""Per-stream verifiers. Each consumes decoded records of ONE stream in ``(run, seq)`` order.

Rules come from real 5-minute samples taken on 2026-09-29 (DATA_DICTIONARY.md):

* spot  ``@trade``      : trade id ``t`` increases by exactly 1 per generation (0 gaps in 10,699 trades).
* usdm  ``@aggTrade``   : aggregate id ``a`` increases by exactly 1 (0 gaps in 4,550). ``f``/``l``
                          adjacency is NOT a criterion: 32 of 4,550 events were not adjacent, so it
                          is only counted as information.
* usdm  ``@markPrice@1s``: event time ``E`` advances ~1000 ms (995 to 1005 ms measured).
* usdm  ``!forceOrder@arr``: notification stream (largest liquidation per 1000 ms per symbol since
                          2026-04-14); only schema and event-time order are checked. It says nothing
                          about total liquidation volume.
* depth (spot / usdm)   : the bridge state machine (``hy_recorder.bridge``).
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any

from ..bridge import BridgeMachine, EventRef
from ..envelope import Record

FAIL = "FAIL"
WARN = "WARN"
INFO = "INFO"

MARK_GAP_FAIL_MS = 3500
MARK_GAP_WARN_MS = 1500
FORCE_EPOCH_WALL_US = 1_776_124_800_000_000  # 2026-04-14T00:00:00Z: semantics changed to "largest"


@dataclass
class Issue:
    code: str
    severity: str
    stream: str
    run: int | None = None
    q: int | None = None
    gen: int | None = None
    detail: dict[str, Any] = field(default_factory=dict)
    explained_by: str | None = None

    def as_dict(self) -> dict[str, Any]:
        d = {
            "code": self.code,
            "severity": self.severity,
            "stream": self.stream,
            "run": self.run,
            "q": self.q,
            "gen": self.gen,
            "detail": self.detail,
        }
        if self.explained_by:
            d["explained_by"] = self.explained_by
        return d


class StreamStats:
    """Coverage bookkeeping shared by all verifiers."""

    def __init__(self) -> None:
        self.records = 0
        self.first_t: int | None = None
        self.last_t: int | None = None
        self.generations: set[tuple[int, int]] = set()
        self.largest_gap_us = 0
        self._prev_t: int | None = None
        self._prev_gen: tuple[int, int] | None = None
        self.fallback_records = 0

    def add(self, rec: Record) -> None:
        self.records += 1
        gen = (rec.run, rec.gen)
        self.generations.add(gen)
        if self.first_t is None:
            self.first_t = rec.wall_us
        if self._prev_t is not None and self._prev_gen == gen:
            self.largest_gap_us = max(self.largest_gap_us, rec.wall_us - self._prev_t)
        self._prev_t, self._prev_gen = rec.wall_us, gen
        self.last_t = rec.wall_us
        if rec.fallback_reason:
            self.fallback_records += 1

    def as_dict(self) -> dict[str, Any]:
        return {
            "records": self.records,
            "first_t": self.first_t,
            "last_t": self.last_t,
            "generations": len(self.generations),
            "largest_gap_s": round(self.largest_gap_us / 1e6, 3),
            "fallback_records": self.fallback_records,
        }


def _data(rec: Record) -> dict[str, Any] | None:
    obj = rec.data
    if not isinstance(obj, dict):
        return None
    inner = obj.get("data", obj)
    return inner if isinstance(inner, dict) else None


class _Base:
    def __init__(self, stream: str) -> None:
        self.stream = stream
        self.stats = StreamStats()
        self.issues: list[Issue] = []
        self.counters: dict[str, int] = {}

    def _issue(self, code: str, severity: str, rec: Record | None, **detail: Any) -> None:
        self.issues.append(
            Issue(
                code,
                severity,
                self.stream,
                rec.run if rec else None,
                rec.seq if rec else None,
                rec.gen if rec else None,
                detail,
            )
        )

    def _count(self, name: str, n: int = 1) -> None:
        self.counters[name] = self.counters.get(name, 0) + n

    def feed(self, rec: Record) -> None:  # pragma: no cover - interface
        raise NotImplementedError

    def finish(self) -> None:
        return None

    def summary(self) -> dict[str, Any]:
        return {"stream": self.stream, **self.stats.as_dict(), "counters": self.counters}


class _IdChain(_Base):
    """Shared logic for integer ids that must advance by exactly 1 per generation."""

    key = "t"
    code = "TRADE_ID"

    def __init__(self, stream: str) -> None:
        super().__init__(stream)
        self._gen: tuple[int, int] | None = None
        self._prev: int | None = None
        self._prev_q: int | None = None

    def _check(self, rec: Record, value: int) -> None:
        gen = (rec.run, rec.gen)
        if gen != self._gen:
            self._gen, self._prev, self._prev_q = gen, None, None
        if self._prev is not None:
            if value == self._prev + 1:
                pass
            elif value > self._prev + 1:
                self._issue(
                    self.code + "_GAP",
                    FAIL,
                    rec,
                    prev=self._prev,
                    got=value,
                    missing=value - self._prev - 1,
                    prev_q=self._prev_q,
                )
                self._count("missing_ids", value - self._prev - 1)
            else:
                self._issue(self.code + "_NON_INCREASING", WARN, rec, prev=self._prev, got=value)
        self._prev = value if self._prev is None else max(self._prev, value)
        self._prev_q = rec.seq

    def feed(self, rec: Record) -> None:
        self.stats.add(rec)
        d = _data(rec)
        if d is None or not isinstance(d.get(self.key), int):
            self._issue(self.code + "_SCHEMA", FAIL, rec, key=self.key)
            return
        self._check(rec, d[self.key])


class TradeVerifier(_IdChain):
    key = "t"
    code = "TRADE_ID"


class AggTradeVerifier(_IdChain):
    key = "a"
    code = "AGGTRADE_ID"

    def __init__(self, stream: str) -> None:
        super().__init__(stream)
        self._prev_l: int | None = None
        self._prev_gen2: tuple[int, int] | None = None

    def feed(self, rec: Record) -> None:
        super().feed(rec)
        d = _data(rec)
        if d is None:
            return
        gen = (rec.run, rec.gen)
        if isinstance(d.get("f"), int) and isinstance(d.get("l"), int):
            if self._prev_gen2 == gen and self._prev_l is not None and d["f"] != self._prev_l + 1:
                self._count("f_l_not_adjacent_info")  # information only, not a criterion
            self._prev_l, self._prev_gen2 = d["l"], gen


class MarkPriceVerifier(_Base):
    def __init__(self, stream: str) -> None:
        super().__init__(stream)
        self._gen: tuple[int, int] | None = None
        self._prev_e: int | None = None
        self._prev_q: int | None = None

    def feed(self, rec: Record) -> None:
        self.stats.add(rec)
        d = _data(rec)
        if d is None or not isinstance(d.get("E"), int):
            self._issue("MARK_SCHEMA", FAIL, rec)
            return
        gen = (rec.run, rec.gen)
        if gen != self._gen:
            self._gen, self._prev_e, self._prev_q = gen, None, None
        e = d["E"]
        if self._prev_e is not None:
            delta = e - self._prev_e
            if delta <= 0:
                self._issue("MARK_NON_MONOTONIC", WARN, rec, prev=self._prev_e, got=e)
            elif delta > MARK_GAP_FAIL_MS:
                self._issue("MARK_CADENCE_GAP", FAIL, rec, delta_ms=delta, prev_q=self._prev_q)
                self._count("missing_seconds", delta // 1000 - 1)
            elif delta > MARK_GAP_WARN_MS:
                self._count("late_updates_info")
        self._prev_e = e
        self._prev_q = rec.seq


class ForceOrderVerifier(_Base):
    REQUIRED = ("s", "S", "q", "p", "ap", "X", "T")

    def __init__(self, stream: str) -> None:
        super().__init__(stream)
        self._gen: tuple[int, int] | None = None
        self._prev_e: int | None = None

    def feed(self, rec: Record) -> None:
        self.stats.add(rec)
        d = _data(rec)
        order = d.get("o") if d else None
        if (
            d is None
            or d.get("e") != "forceOrder"
            or not isinstance(d.get("E"), int)
            or not isinstance(order, dict)
            or any(k not in order for k in self.REQUIRED)
        ):
            self._issue("FORCEORDER_SCHEMA", FAIL, rec)
            return
        if rec.wall_us < FORCE_EPOCH_WALL_US:
            self._issue("FORCEORDER_PRE_EPOCH", WARN, rec)
        gen = (rec.run, rec.gen)
        if gen != self._gen:
            self._gen, self._prev_e = gen, None
        if self._prev_e is not None and d["E"] < self._prev_e:
            self._issue("FORCEORDER_NON_MONOTONIC", WARN, rec, prev=self._prev_e, got=d["E"])
        self._prev_e = d["E"] if self._prev_e is None else max(self._prev_e, d["E"])


class DepthVerifier(_Base):
    """Bridge-state verification for one ``(venue, symbol)`` depth stream.

    Snapshots (``k == 's'`` records of the matching snapshot stream) are injected in ``(run, seq)``
    order through :meth:`feed_snapshot`. Output: verified intervals and issues.
    """

    def __init__(self, stream: str, venue: str) -> None:
        super().__init__(stream)
        self.venue = venue
        self.machine = BridgeMachine(venue)
        self._gen: tuple[int, int] | None = None
        self.intervals: list[dict[str, Any]] = []
        self._open: dict[str, Any] | None = None
        self.unverified_gens: list[dict[str, Any]] = []
        self._gen_has_bridge = False
        self._gen_first: tuple[int, int, int] | None = None  # (run, q, t)
        self._last_q: int | None = None

    # -- interval bookkeeping ----------------------------------------------------
    def _close_interval(self, end_rec: tuple[int, int] | None) -> None:
        if self._open is not None:
            if end_rec is not None:
                self._open["end_q"], self._open["end_t"] = end_rec
            self.intervals.append(self._open)
            self._open = None

    def _end_generation(self) -> None:
        self._close_interval(None)
        if self._gen is not None and not self._gen_has_bridge and self._gen_first is not None:
            self.unverified_gens.append(
                {"run": self._gen[0], "gen": self._gen[1], "first_q": self._gen_first[1]}
            )

    def _new_generation(self, rec: Record) -> None:
        self._end_generation()
        self._gen = (rec.run, rec.gen)
        self._gen_has_bridge = False
        self._gen_first = (rec.run, rec.seq, rec.wall_us)
        self.machine.reset()

    # -- input -------------------------------------------------------------------
    def feed(self, rec: Record) -> None:
        self.stats.add(rec)
        d = _data(rec)
        if (
            d is None
            or not isinstance(d.get("U"), int)
            or not isinstance(d.get("u"), int)
            or (self.venue == "usdm" and not isinstance(d.get("pu"), int))
        ):
            self._issue("DEPTH_SCHEMA", FAIL, rec)
            return
        if (rec.run, rec.gen) != self._gen:
            self._new_generation(rec)
        ev = EventRef(rec.seq, d["U"], d["u"], d.get("pu"), rec.wall_us)
        for tr in self.machine.on_event(ev):
            self._on_transition(tr, rec)
        self._last_q = rec.seq
        if self._open is not None:
            self._open["end_q"], self._open["end_t"] = rec.seq, rec.wall_us

    def feed_snapshot(self, rec: Record) -> None:
        obj = rec.data
        L = obj.get("lastUpdateId") if isinstance(obj, dict) else None
        if not isinstance(L, int):
            self._issue("SNAPSHOT_SCHEMA", FAIL, rec)
            return
        if self._gen is None or (rec.run, rec.gen) != self._gen:
            # Another generation's snapshot (the connection was replaced while the request was in flight).
            self._count("snapshot_other_generation_info")
            return
        self._count("snapshots")
        for tr in self.machine.on_snapshot(L, rec.seq):
            self._on_transition(tr, rec)

    def _on_transition(self, tr, rec: Record) -> None:
        if tr.kind == "BRIDGED":
            self._gen_has_bridge = True
            t0 = tr.detail.get("t") or rec.wall_us  # the bridging event's own time, not the snapshot's
            self._open = {
                "run": rec.run,
                "gen": rec.gen,
                "start_q": tr.q,
                "end_q": tr.q,
                "start_t": t0,
                "end_t": t0,
                "L": tr.detail["L"],
            }
            self._count("bridges")
        elif tr.kind == "GAP":
            self._close_interval(None)  # the verified interval ends at the previous event
            # the issue belongs to the event that exposed the gap (tr.q), which may not be ``rec``
            self.issues.append(
                Issue(
                    "DEPTH_GAP",
                    FAIL,
                    self.stream,
                    rec.run,
                    tr.q,
                    rec.gen,
                    {"prev_q": self._last_q, **tr.detail},
                )
            )
            self._count("gaps")
        elif tr.kind == "SNAPSHOT_STALE":
            self._count("stale_snapshots")
        elif tr.kind == "OVERLAP":
            self._count("overlap_info")
        elif tr.kind == "KEYFRAME":
            self._count("keyframes")
            if not tr.detail.get("covered"):
                self.issues.append(
                    Issue(
                        "KEYFRAME_UNCOVERED",
                        WARN,
                        self.stream,
                        rec.run,
                        tr.q,
                        rec.gen,
                        {"L": tr.detail.get("L")},
                    )
                )

    def finish(self) -> None:
        self._end_generation()
        self._gen = None

    def summary(self) -> dict[str, Any]:
        verified_us = sum(max(0, i["end_t"] - i["start_t"]) for i in self.intervals)
        first, last = self.stats.first_t, self.stats.last_t
        span_us = (last - first) if first is not None and last is not None else 0
        return {
            **super().summary(),
            "venue": self.venue,
            "verified_intervals": len(self.intervals),
            "verified_seconds": round(verified_us / 1e6, 3),
            "span_seconds": round(span_us / 1e6, 3),
            "unverified_generations": len(self.unverified_gens),
        }
