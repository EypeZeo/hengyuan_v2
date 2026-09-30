"""Depth snapshot scheduling, decoupled from the read loop.

* **Generation start**: when a new connection generation has buffered its first depth event, the
  scheduler requests ONE snapshot immediately (subject to a 5 second hard cooldown per
  ``(venue, symbol)`` and the REST budget). The reader never waits for REST.
* **Failure** (HTTP error, timeout, budget denial, a stale snapshot): retry with exponential backoff
  (5 s, doubling, capped at 5 minutes) and **no attempt cap** until the generation is bridged or ends.
  A stale snapshot (``lastUpdateId`` below the first buffered ``U``) is normal and retried after the
  cooldown without growing the backoff. A 429 puts the governor into deep sleep; the scheduler simply
  keeps asking and is told to wait.
* **Keyframes**: one snapshot per hour per stream for random access; skipped (not caught up) when REST
  is sleeping or over budget.

Snapshots that complete after the generation changed are discarded, never attached to the new one.
"""

from __future__ import annotations

import asyncio
import hashlib
import json
from collections.abc import Awaitable, Callable
from dataclasses import dataclass
from typing import Any

from .clock import Clock
from .depthtrack import DepthTracker
from .guard import ForbiddenEndpoint, depth_weight
from .ratelimit import RestGovernor
from .rest import RestError, RestResult

BACKOFF_START_S = 5.0
BACKOFF_CAP_S = 300.0
KEYFRAME_RETRY_S = 300.0
STILL_BRIDGING_RECHECK_S = 10.0


@dataclass
class _Key:
    venue: str
    symbol: str
    gen: int = 0
    pending: bool = False
    ready_at: float = 0.0
    backoff: float = 0.0
    last_request: float = -1e9
    next_keyframe: float = 0.0
    inflight: bool = False
    last_defer_emit: float = -1e9


Fetch = Callable[[str, str, dict[str, object]], Awaitable[RestResult]]
Sink = Callable[[str, str, int, bytes], "int | None"]


class SnapshotScheduler:
    def __init__(
        self,
        *,
        clock: Clock,
        governor: RestGovernor,
        fetch: Fetch,
        sink: Sink,
        tracker_for: Callable[[str, str], DepthTracker],
        emit: Callable[..., None],
        targets: tuple[tuple[str, str], ...],
        limit: int = 1000,
        cooldown_s: float = 5.0,
        keyframe_interval_s: float = 3600.0,
    ) -> None:
        self._clock = clock
        self._gov = governor
        self._fetch = fetch
        self._sink = sink
        self._tracker_for = tracker_for
        self._emit = emit
        self._limit = limit
        self._cooldown = cooldown_s
        self._keyframe_interval = keyframe_interval_s
        self._keys = {t: _Key(*t) for t in targets}
        self._tasks: set[asyncio.Task[None]] = set()
        self._stopping = False

    # -- hooks called by the connection manager / tracker (event loop thread) --------------
    def generation_started(self, venue: str, symbol: str, gen: int) -> None:
        key = self._keys.get((venue, symbol))
        if key is not None:
            key.gen, key.pending, key.backoff, key.ready_at, key.next_keyframe = gen, False, 0.0, 0.0, 0.0

    def first_event(self, venue: str, symbol: str, gen: int) -> None:
        key = self._keys.get((venue, symbol))
        if key is not None and key.gen == gen:
            key.pending, key.ready_at = True, self._clock.mono_s()

    def bridged(self, venue: str, symbol: str, gen: int) -> None:
        key = self._keys.get((venue, symbol))
        if key is not None and key.gen == gen:
            key.pending, key.backoff = False, 0.0
            key.next_keyframe = self._clock.mono_s() + self._keyframe_interval

    def state_of(self, venue: str, symbol: str) -> dict[str, Any]:
        k = self._keys[(venue, symbol)]
        return {"gen": k.gen, "pending": k.pending, "backoff_s": k.backoff, "inflight": k.inflight}

    # -- scheduling ------------------------------------------------------------------------
    async def tick(self) -> None:
        now = self._clock.mono_s()
        for key in self._keys.values():
            if key.inflight or key.gen == 0:
                continue
            if key.pending and now >= key.ready_at and now - key.last_request >= self._cooldown:
                self._spawn(key, "generation_start")
            elif not key.pending and key.next_keyframe > 0 and now >= key.next_keyframe:
                self._spawn(key, "keyframe")

    async def run(self, interval_s: float = 0.25) -> None:
        while not self._stopping:
            await self.tick()
            await asyncio.sleep(interval_s)

    async def wait_idle(self) -> None:
        while self._tasks:
            await asyncio.gather(*list(self._tasks), return_exceptions=True)

    async def stop(self) -> None:
        self._stopping = True
        for task in list(self._tasks):
            task.cancel()
        await asyncio.gather(*list(self._tasks), return_exceptions=True)

    def _spawn(self, key: _Key, trigger: str) -> None:
        key.inflight = True
        task = asyncio.get_running_loop().create_task(self._run_attempt(key, trigger))
        self._tasks.add(task)
        task.add_done_callback(self._tasks.discard)

    async def _run_attempt(self, key: _Key, trigger: str) -> None:
        try:
            await self._attempt(key, trigger)
        finally:
            key.inflight = False

    # -- one attempt -----------------------------------------------------------------------
    async def _attempt(self, key: _Key, trigger: str) -> None:
        venue, symbol = key.venue, key.symbol
        stream = "%s:snapshot:%s" % (venue, symbol)
        weight = depth_weight(venue, self._limit)
        now = self._clock.mono_s()
        permit = self._gov.permit(venue, weight)
        if not permit.ok:
            wait = max(permit.retry_after_s, 1.0)
            if trigger == "keyframe":
                key.next_keyframe = now + max(wait, 60.0)  # keyframes are skippable: do not hammer
            else:
                key.ready_at = now + wait
            if now - key.last_defer_emit >= 60.0:
                key.last_defer_emit = now
                self._emit(
                    "SNAPSHOT_DEFERRED",
                    stream=stream,
                    gen=key.gen,
                    trigger=trigger,
                    reason=permit.reason,
                    wait_s=round(wait, 1),
                )
            return
        self._gov.spent(venue, weight)
        key.last_request = now
        gen = key.gen
        try:
            res = await self._fetch(venue, "depth", {"symbol": symbol, "limit": self._limit})
        except RestError as exc:
            self._fail(key, trigger, gen, stream, reason="transport", detail=exc.kind)
            return
        except ForbiddenEndpoint as exc:  # a bug: never retry hot
            self._fail(key, trigger, gen, stream, reason="forbidden", detail=str(exc))
            return
        self._gov.on_response(venue, res.status, res.headers)
        if res.status != 200:
            self._fail(key, trigger, gen, stream, reason="http", status=res.status)
            return
        try:
            L = int(json.loads(res.body)["lastUpdateId"])
        except (ValueError, KeyError, TypeError):
            self._fail(key, trigger, gen, stream, reason="bad_body")
            return
        if key.gen != gen:
            self._emit(
                "SNAPSHOT_DISCARDED", stream=stream, gen=gen, current_gen=key.gen, trigger=trigger, L=L
            )
            return
        seq = self._sink(venue, symbol, gen, res.body)
        if seq is None:  # the writer queue refused it (already accounted as OVERRUN there)
            self._fail(key, trigger, gen, stream, reason="dropped")
            return
        self._emit(
            "SNAPSHOT",
            stream=stream,
            gen=gen,
            trigger=trigger,
            status=res.status,
            limit=self._limit,
            weight=weight,
            used_weight_1m=res.headers.get("x-mbx-used-weight-1m"),
            req_start_mono_us=res.start_mono_us,
            req_end_mono_us=res.end_mono_us,
            last_update_id=L,
            bytes=len(res.body),
            sha256=hashlib.sha256(res.body).hexdigest(),
            rec_q=seq,
        )
        trs = self._tracker_for(venue, symbol).on_snapshot(gen, L, seq)
        kinds = [t.kind for t in trs]
        after = self._clock.mono_s()
        if trigger == "keyframe":
            key.next_keyframe = after + self._keyframe_interval
            return
        if "BRIDGED" in kinds:
            return  # the tracker callback already told us (bridged())
        if "SNAPSHOT_STALE" in kinds:
            key.ready_at = (
                after + self._cooldown
            )  # normal: try again after the hard cooldown, backoff unchanged
        else:
            key.ready_at = (
                after + STILL_BRIDGING_RECHECK_S
            )  # snapshot is ahead of the buffered events; they will bridge it

    def _fail(self, key: _Key, trigger: str, gen: int, stream: str, **detail: Any) -> None:
        now = self._clock.mono_s()
        if trigger == "keyframe":
            key.next_keyframe = now + KEYFRAME_RETRY_S
            backoff = KEYFRAME_RETRY_S
        else:
            key.backoff = min(BACKOFF_CAP_S, max(BACKOFF_START_S, key.backoff * 2))
            key.ready_at = now + key.backoff
            backoff = key.backoff
        self._emit("SNAPSHOT_FAIL", stream=stream, gen=gen, trigger=trigger, backoff_s=backoff, **detail)
