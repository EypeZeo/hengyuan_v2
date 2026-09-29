"""One WebSocket connection: read loop, per-stream liveness, generations, reconnect policy.

Design rules (see README and the plan):

* The receive path only stamps time, draws a sequence number, encodes the record and calls
  ``Writer.submit_record`` (``put_nowait``). It never waits for disk, compression or REST.
* Every successful connect is a new **generation**; ``WS_OPEN`` is written to the ledger before any
  record of that generation. A depth gap or a stalled stream reconnects only this connection.
* Silence is classified, not lumped together: a stream that had data and then went quiet is a
  ``STREAM_STALL`` (reconnect this connection); a stream that never delivered after subscribing is
  ``SUBSCRIBED_NO_DATA`` (a routing/path problem: flagged unhealthy, retried only every 5 minutes);
  a DNS delay is recorded, not acted on.
* Reconnects use exponential backoff with jitter, are spaced while REST is in deep sleep, stop
  entirely during a ban (HTTP 418), and are capped at 30 attempts per 5 minutes.
"""

from __future__ import annotations

import asyncio
import json
import random
import re
import socket
from collections import deque
from collections.abc import Callable
from dataclasses import dataclass, field
from typing import Any
from urllib.parse import urlsplit

from websockets.asyncio.client import connect as ws_connect

from . import __version__
from .clock import Clock
from .config import ConnectionSpec, StreamSpec
from .depthtrack import DepthTracker
from .envelope import encode_record
from .ratelimit import RestGovernor
from .seq import SeqCounter
from .snapshots import SnapshotScheduler
from .writer import Writer

_STREAM_ID_RE = re.compile(r"^[A-Za-z0-9_.@!\-]{1,90}$")
NO_DATA_RETRY_S = 300.0
ATTEMPT_WINDOW_S = 300.0
MAX_ATTEMPTS_PER_WINDOW = 30
BAD_FRAME_EMITS_PER_MIN = 10
_USER_AGENT = "hy-recorder/%s (read-only public data)" % __version__


@dataclass
class SessionEnv:
    clock: Clock
    seq: SeqCounter
    run_no: int
    writer: Writer
    emit: Callable[..., None]  # ledger event: emit(kind, **fields)
    governor: RestGovernor
    stop: asyncio.Event
    trackers: dict[str, DepthTracker] = field(default_factory=dict)
    scheduler: SnapshotScheduler | None = None
    connect: Callable[..., Any] = ws_connect
    rng: random.Random = field(default_factory=random.Random)
    rotation_age_s: float = 23.5 * 3600
    forced_reconnect_cooldown_s: float = 60.0
    idle_poll_s: float = 1.0
    open_timeout_s: float = 15.0
    measure_dns: bool = True  # resolve once before connecting, only to report slow DNS (tests switch it off)
    healthy_after_s: float = (
        60.0  # a connection that lived this long reconnects at once; a shorter one backs off
    )
    backoff_start_s: float = 1.0
    backoff_cap_s: float = 60.0


class _Watch:
    def __init__(self, spec: StreamSpec) -> None:
        self.spec = spec
        self.last_seen: float | None = None
        self.subscribed = 0.0
        self.no_data_emitted = False
        self.stalled = False
        self.frames = 0

    def reset(self, now: float) -> None:
        self.last_seen, self.subscribed = None, now
        self.no_data_emitted = self.stalled = False
        self.frames = 0

    def touch(self, now: float) -> None:
        self.last_seen = now
        self.frames += 1


class ConnectionManager:
    def __init__(self, spec: ConnectionSpec, env: SessionEnv) -> None:
        self.spec = spec
        self.ctx = env
        self.gen = 0
        self.state = "init"
        self.frames_total = 0
        self.opened_mono: float | None = None
        self._watch = {spec.stream_id(s.name): _Watch(s) for s in spec.streams}
        self._reconnect_reason: str | None = None
        self._deferred_reason: str | None = None
        self._last_forced = -1e9
        self._bad_frame_window = deque[float]()
        self._unexpected_seen: set[str] = set()
        self._parts = urlsplit(spec.url)

    # -- external requests -------------------------------------------------------------------
    def request_reconnect(self, reason: str) -> bool:
        """Ask for a reconnect of this connection (e.g. a depth gap). Rate-limited so a flapping
        stream cannot cause a reconnect storm; a refused request is applied when the cooldown ends."""
        now = self.ctx.clock.mono_s()
        if now - self._last_forced < self.ctx.forced_reconnect_cooldown_s:
            self._deferred_reason = reason
            return False
        self._reconnect_reason = reason
        self._last_forced = now
        return True

    # -- main loop ---------------------------------------------------------------------------
    async def run(self) -> None:
        env, clock = self.ctx, self.ctx.clock
        attempts: deque[float] = deque()
        backoff = env.backoff_start_s
        while not env.stop.is_set():
            wait = self._gate(attempts)
            if wait > 0:
                self.state = "waiting"
                await self._sleep(min(wait, 1.0))
                continue
            attempts.append(clock.mono_s())
            started = clock.mono_s()
            self.state = "connecting"
            opened = False
            try:
                reason = await self._connect_and_read()
                opened = True
            except asyncio.CancelledError:
                raise
            except Exception as exc:  # noqa: BLE001 - every network/protocol failure ends up here
                reason = "error:%s" % type(exc).__name__
                self._on_connect_error(exc)
            healthy = opened and clock.mono_s() - started >= env.healthy_after_s
            backoff = env.backoff_start_s if healthy else min(backoff * 2, env.backoff_cap_s)
            self.state = "backoff"
            if not env.stop.is_set():
                # a connection that lived a while reconnects at once (the data hole is already there);
                # one that failed quickly backs off exponentially so a broken path cannot become a storm
                delay = 0.2 if healthy or reason == "rotation" else backoff
                await self._sleep(delay * (0.5 + env.rng.random()))
        self.state = "stopped"

    async def _sleep(self, seconds: float) -> None:
        try:
            await asyncio.wait_for(self.ctx.stop.wait(), timeout=seconds)
        except TimeoutError:
            pass

    def _gate(self, attempts: deque[float]) -> float:
        gov, now = self.ctx.governor, self.ctx.clock.mono_s()
        ban = gov.ws_blocked_s()
        if ban > 0:
            return ban
        spacing = gov.ws_min_spacing_s()
        if spacing > 0 and attempts and now - attempts[-1] < spacing:
            return spacing - (now - attempts[-1])
        while attempts and now - attempts[0] > ATTEMPT_WINDOW_S:
            attempts.popleft()
        if len(attempts) >= MAX_ATTEMPTS_PER_WINDOW:
            return ATTEMPT_WINDOW_S - (now - attempts[0])
        return 0.0

    def _on_connect_error(self, exc: Exception) -> None:
        response = getattr(exc, "response", None)
        status = getattr(response, "status_code", None)
        if status in (403, 418, 429, 451):
            headers = (
                {k.lower(): v for k, v in getattr(response, "headers", {}).items()}
                if response is not None
                else {}
            )
            self.ctx.governor.on_response(self.spec.venue, status, headers)
        self.ctx.emit(
            "WS_CONNECT_FAIL",
            conn=self.spec.name,
            error=type(exc).__name__,
            status=status,
            detail=str(exc)[:120],
        )

    # -- one connection ----------------------------------------------------------------------
    async def _connect_and_read(self) -> str:
        env, spec, clock = self.ctx, self.spec, self.ctx.clock
        host, port = self._parts.hostname or "", self._parts.port or 443
        t0 = clock.mono_s()
        dns_ms = -1.0
        if env.measure_dns:
            try:
                await asyncio.wait_for(
                    asyncio.get_running_loop().getaddrinfo(host, port, type=socket.SOCK_STREAM), 10
                )
                dns_ms = (clock.mono_s() - t0) * 1000
            except (OSError, TimeoutError):
                pass  # connect() will report the real error
        if dns_ms > 1000:
            env.emit("DNS_SLOW", conn=spec.name, host=host, dns_ms=round(dns_ms))
        t1 = clock.mono_s()
        async with env.connect(
            spec.url,
            open_timeout=env.open_timeout_s,
            close_timeout=5,
            ping_interval=None,
            ping_timeout=None,
            max_size=8 * 1024 * 1024,
            compression=None,
            proxy=None,
            user_agent_header=_USER_AGENT,
        ) as ws:
            connect_ms = (clock.mono_s() - t1) * 1000
            self.gen += 1
            gen = self.gen
            opened = clock.mono_s()
            self.opened_mono = opened
            self._begin_generation(gen, opened, connect_ms, dns_ms)
            self.state = "open"
            reason = "unknown"
            try:
                reason = await self._read_loop(ws, gen, opened)
            except asyncio.CancelledError:
                reason = "cancelled"
                raise
            except Exception as exc:  # noqa: BLE001 - a failure after the handshake is a read error, not a connect failure
                reason = "read_error:%s" % type(exc).__name__
            finally:
                self._end_generation(gen, reason, opened)
                self.state = "closing"
            return reason

    def _begin_generation(self, gen: int, now: float, connect_ms: float, dns_ms: float) -> None:
        env, spec = self.ctx, self.spec
        for w in self._watch.values():
            w.reset(now)
        self._unexpected_seen.clear()
        self._reconnect_reason = self._deferred_reason = None
        for sid in spec.stream_ids:
            tracker = env.trackers.get(sid)
            if tracker is not None:
                tracker.new_generation(gen)
                if env.scheduler is not None:
                    env.scheduler.generation_started(tracker.venue, tracker.symbol, gen)
        env.emit(
            "WS_OPEN",
            conn=spec.name,
            gen=gen,
            streams=list(spec.stream_ids),
            url_path=self._parts.path,
            connect_ms=round(connect_ms, 1),
            dns_ms=round(dns_ms, 1),
        )

    def _end_generation(self, gen: int, reason: str, opened: float) -> None:
        self.ctx.emit(
            "WS_CLOSE",
            conn=self.spec.name,
            gen=gen,
            reason=reason,
            frames=sum(w.frames for w in self._watch.values()),
            duration_s=round(self.ctx.clock.mono_s() - opened, 1),
        )

    async def _read_loop(self, ws: Any, gen: int, opened: float) -> str:
        env, clock = self.ctx, self.ctx.clock
        last_check = clock.mono_s()
        while True:
            if env.stop.is_set():
                return "stop"
            try:
                async with asyncio.timeout(env.idle_poll_s):
                    msg = await ws.recv()
            except TimeoutError:
                reason = self._periodic_checks(opened)
                if reason:
                    return reason
                last_check = clock.mono_s()
                continue
            except Exception as exc:  # noqa: BLE001 - ConnectionClosed and friends
                if exc.__class__.__name__.startswith("ConnectionClosed"):
                    code = getattr(getattr(exc, "rcvd", None), "code", None)
                    return "closed:%s" % code
                raise
            self._on_message(msg, gen)
            now = clock.mono_s()
            # a request raised while handling this very frame (a depth gap) is acted on at once; on a busy
            # stream the idle timeout above never fires, so the periodic checks also run on a timer
            if self._reconnect_reason is not None or now - last_check >= env.idle_poll_s:
                last_check = now
                reason = self._periodic_checks(opened)
                if reason:
                    return reason

    # -- liveness ----------------------------------------------------------------------------
    def _periodic_checks(self, opened: float) -> str | None:
        env = self.ctx
        now = env.clock.mono_s()
        if now - opened >= env.rotation_age_s:
            return "rotation"
        if self._deferred_reason and now - self._last_forced >= env.forced_reconnect_cooldown_s:
            self._reconnect_reason, self._deferred_reason = self._deferred_reason, None
            self._last_forced = now
        if self._reconnect_reason:
            reason, self._reconnect_reason = self._reconnect_reason, None
            return reason
        for sid, w in self._watch.items():
            spec = w.spec
            if spec.max_gap_s is None:
                continue
            if w.last_seen is None:
                waited = now - w.subscribed
                if not w.no_data_emitted and waited >= (spec.first_frame_s or 30.0):
                    w.no_data_emitted = True
                    env.emit(
                        "SUBSCRIBED_NO_DATA",
                        conn=self.spec.name,
                        gen=self.gen,
                        stream=sid,
                        waited_s=round(waited, 1),
                    )
                if waited >= NO_DATA_RETRY_S:
                    return "no_data_retry"
            elif now - w.last_seen >= spec.max_gap_s and not w.stalled:
                w.stalled = True
                env.emit(
                    "STREAM_STALL",
                    conn=self.spec.name,
                    gen=self.gen,
                    stream=sid,
                    silent_s=round(now - w.last_seen, 1),
                    max_gap_s=spec.max_gap_s,
                )
                return "stall:%s" % sid
        return None

    # -- receive path (never blocks) -----------------------------------------------------------
    def _on_message(self, msg: str | bytes, gen: int) -> None:
        env, spec, clock = self.ctx, self.spec, self.ctx.clock
        wall, mono = clock.wall_us(), clock.mono_us()
        seq = env.seq.next()
        binary = isinstance(msg, (bytes, bytearray))
        obj: Any = None
        if not binary:
            try:
                obj = json.loads(msg)
            except ValueError:
                obj = None
        name = obj.get("stream") if isinstance(obj, dict) else None
        if isinstance(name, str) and _STREAM_ID_RE.match(name):
            sid = spec.stream_id(name)
        else:
            sid = "%s:unknown" % spec.venue
        line, reason = encode_record(
            run=env.run_no,
            seq=seq,
            gen=gen,
            wall_us=wall,
            mono_us=mono,
            stream=sid,
            payload=bytes(msg) if binary else msg,
            binary=binary,
        )
        watch = self._watch.get(sid)
        if reason or sid.endswith(":unknown"):
            self._note_bad_frame(sid, reason or "no_stream_field", seq)
        elif watch is None and sid not in self._unexpected_seen:
            self._unexpected_seen.add(sid)
            env.emit("UNEXPECTED_STREAM", conn=spec.name, gen=gen, stream=sid)
        accepted = env.writer.submit_record(spec.venue, spec.cls, line, seq=seq, wall_us=wall, stream=sid)
        self.frames_total += 1
        if watch is not None:
            watch.touch(clock.mono_s())
        if accepted and watch is not None and spec.is_depth:
            tracker = env.trackers.get(sid)
            data = obj.get("data") if isinstance(obj, dict) else None
            if tracker is not None and isinstance(data, dict):
                U, u, pu = data.get("U"), data.get("u"), data.get("pu")
                if isinstance(U, int) and isinstance(u, int) and (pu is None or isinstance(pu, int)):
                    tracker.on_event(seq, U, u, pu, wall)
                else:
                    self._note_bad_frame(sid, "depth_schema", seq)

    def _note_bad_frame(self, sid: str, reason: str, seq: int) -> None:
        now = self.ctx.clock.mono_s()
        window = self._bad_frame_window
        while window and now - window[0] > 60:
            window.popleft()
        if len(window) < BAD_FRAME_EMITS_PER_MIN:
            window.append(now)
            self.ctx.emit(
                "BAD_FRAME", conn=self.spec.name, gen=self.gen, stream=sid, reason=reason, bad_q=seq
            )

    # -- observability -----------------------------------------------------------------------
    def snapshot(self) -> dict[str, Any]:
        now = self.ctx.clock.mono_s()
        return {
            "state": self.state,
            "gen": self.gen,
            "frames": self.frames_total,
            "age_s": round(now - self.opened_mono, 1)
            if self.opened_mono is not None and self.state == "open"
            else None,
            "streams": {
                sid: {
                    "frames": w.frames,
                    "last_frame_age_s": None if w.last_seen is None else round(now - w.last_seen, 2),
                    "healthy": not (w.no_data_emitted or w.stalled),
                }
                for sid, w in self._watch.items()
            },
        }
