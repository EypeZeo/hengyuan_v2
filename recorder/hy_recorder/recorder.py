"""Recorder orchestration: startup, task supervision, graceful shutdown.

Threads: the asyncio event loop (WebSocket receive, REST scheduling, timers), the single writer
thread (:class:`~hy_recorder.writer.Writer`) and the monitor thread (:class:`~hy_recorder.monitor.Monitor`).
REST calls run in worker threads via ``asyncio.to_thread``, one in-flight call per venue.

Nothing here reads credentials, signs anything, or contacts a private/trading endpoint; the only
network destinations are the ones :mod:`hy_recorder.guard` can build.
"""

from __future__ import annotations

import asyncio
import faulthandler
import json
import os
import platform
import random
import signal
import sys
import time
from collections.abc import Awaitable, Callable
from pathlib import Path
from typing import Any

import websockets
import zstandard

from . import __version__
from .clock import SYSTEM_CLOCK, Clock
from .config import RecorderConfig
from .depthtrack import DepthTracker
from .envelope import KIND_SNAPSHOT, encode_record
from .guard import endpoint_weight
from .ledger import Ledger, make_event
from .manifest import Manifest
from .monitor import Monitor
from .ratelimit import RestGovernor
from .rest import RestClient, RestError, RestResult
from .retention import ack_dir
from .segment import recover
from .seq import SeqCounter
from .session import ConnectionManager, SessionEnv
from .snapshots import SnapshotScheduler
from .state import StateFile
from .writer import Writer

EXIT_WRITER_DEAD = 72
WRITER_JOIN_TIMEOUT_S = 30.0
CONN_STOP_TIMEOUT_S = 20.0
REF_RETRY_S = 300.0


def host_profile() -> dict[str, Any]:
    """Non-identifying host facts for the ledger (no host name, no addresses)."""
    prof: dict[str, Any] = {
        "system": platform.system(),
        "kernel": platform.release(),
        "machine": platform.machine(),
        "cpus": os.cpu_count(),
        "python": platform.python_version(),
        "recorder": __version__,
        "websockets": getattr(websockets, "__version__", "?"),
        "zstandard": getattr(zstandard, "__version__", "?"),
    }
    try:
        for line in Path("/proc/meminfo").read_text().splitlines():
            if line.startswith("MemTotal:"):
                prof["mem_total_kb"] = int(line.split()[1])
                break
    except (OSError, ValueError):
        pass
    return prof


class Recorder:
    def __init__(
        self,
        cfg: RecorderConfig,
        *,
        clock: Clock = SYSTEM_CLOCK,
        connect: Callable[..., Any] | None = None,
        rest_conn_factory: Callable[[str], Any] | None = None,
        log: Callable[[str], None] | None = None,
        rng: random.Random | None = None,
        session_overrides: dict[str, Any] | None = None,
        exchange_info_delay_s: float = 90.0,
        enable_monitor: bool = True,
        monitor_exit: Callable[[int], None] = os._exit,
    ) -> None:
        self.cfg = cfg
        self.clock = clock
        self._connect = connect
        self._rest_conn_factory = rest_conn_factory
        self._log = log or (lambda msg: print("hy-recorder: " + msg, file=sys.stderr, flush=True))
        self._rng = rng or random.Random()
        self._session_overrides = dict(session_overrides or {})
        self._exchange_info_delay_s = exchange_info_delay_s
        self._enable_monitor = enable_monitor
        self._monitor_exit = monitor_exit
        # populated by run()
        self.state: StateFile
        self.run_no = 0
        self.seq = SeqCounter()
        self.manifest: Manifest
        self.ledger: Ledger
        self.writer: Writer
        self.governor: RestGovernor
        self.scheduler: SnapshotScheduler
        self.managers: dict[str, ConnectionManager] = {}
        self.trackers: dict[str, DepthTracker] = {}
        self._tracker_by_target: dict[tuple[str, str], DepthTracker] = {}
        self._rest: RestClient
        self._rest_locks: dict[str, asyncio.Lock] = {}
        self.loop_hb = time.monotonic()
        self.max_lag_s = 0.0
        self.exit_code = 0
        self.monitor: Monitor
        self._stop: asyncio.Event

    # -- ledger events ------------------------------------------------------------------------
    def emit(self, kind: str, **fields: Any) -> None:
        ev = make_event(
            self.run_no, self.seq.next(), self.clock.wall_us(), self.clock.mono_us(), kind, **fields
        )
        self.writer.submit_event(ev)

    def _status_extra(self) -> dict[str, Any]:
        now = time.monotonic()
        return {
            "version": __version__,
            "fingerprint": self.cfg.fingerprint(),
            "connections": {n: m.snapshot() for n, m in self.managers.items()},
            "trackers": {sid: {"state": t.state, "gen": t.gen} for sid, t in self.trackers.items()},
            "snapshots": {"%s:%s" % k: self.scheduler.state_of(*k) for k in self.cfg.depth_targets},
            "rest": self.governor.snapshot(),
            "loop": {"hb_age_s": round(now - self.loop_hb, 2), "max_lag_s": round(self.max_lag_s, 3)},
        }

    # -- startup (synchronous) ----------------------------------------------------------------
    def _boot(self) -> None:
        cfg = self.cfg
        root = Path(cfg.root)
        root.mkdir(parents=True, exist_ok=True)
        ack_dir(root).mkdir(exist_ok=True)
        self.state = StateFile(root)
        self.state.load()
        self.run_no, prev_clean = self.state.begin_run()
        self.manifest = Manifest(root)
        self.ledger = Ledger(root)
        self.writer = Writer(
            cfg,
            state=self.state,
            manifest=self.manifest,
            ledger=self.ledger,
            clock=self.clock,
            seq=self.seq,
            run_no=self.run_no,
            status_extra=self._status_extra,
        )
        self.emit(
            "PROC_START",
            version=__version__,
            pid=os.getpid(),
            prev_clean=prev_clean,
            state_recovered=self.state.recovered,
            fingerprint=cfg.fingerprint(),
            config={
                "symbols": list(cfg.symbols),
                "zstd_level": cfg.zstd_level,
                "queue_capacity": cfg.queue_capacity,
                "snapshot_limit": cfg.snapshot_limit,
                "retain_hours": cfg.retain_hours,
                "connections": [c.name for c in cfg.connections],
            },
        )
        self.emit("HOST_PROFILE", **host_profile())
        try:
            recover(
                root,
                manifest=self.manifest,
                level=cfg.zstd_level,
                next_segseq=self.state.next_segseq,
                clock=self.clock,
                emit=self.emit,
            )
        except Exception as exc:  # noqa: BLE001 - a recovery problem must not become a crash loop with no data
            self.emit("RECOVERY_FAILED", error=type(exc).__name__, detail=str(exc)[:200])
            self._log("recovery failed: %s: %s" % (type(exc).__name__, exc))
        self.writer.start()
        self.loop_hb = (
            time.monotonic()
        )  # boot is synchronous on the loop thread; the monitor starts counting now
        if self._enable_monitor:
            self.monitor = Monitor(
                lambda: self.loop_hb, lambda: self.writer.hb_mono, exit_fn=self._monitor_exit
            )
            self.monitor.start()

    # -- REST plumbing --------------------------------------------------------------------------
    async def _fetch(self, venue: str, endpoint: str, params: dict[str, object]) -> RestResult:
        async with self._rest_locks[venue]:  # http.client connections are not thread-safe: one call per venue
            return await asyncio.to_thread(self._rest.get, venue, endpoint, params)

    def _submit_body(self, venue: str, cls: str, stream: str, gen: int, body: bytes) -> int | None:
        seq = self.seq.next()
        wall, mono = self.clock.wall_us(), self.clock.mono_us()
        line, _ = encode_record(
            run=self.run_no,
            seq=seq,
            gen=gen,
            wall_us=wall,
            mono_us=mono,
            stream=stream,
            payload=body,
            kind=KIND_SNAPSHOT,
        )
        return (
            seq if self.writer.submit_record(venue, cls, line, seq=seq, wall_us=wall, stream=stream) else None
        )

    def _sink_snapshot(self, venue: str, symbol: str, gen: int, body: bytes) -> int | None:
        return self._submit_body(venue, "snapshot", "%s:snapshot:%s" % (venue, symbol), gen, body)

    # -- helpers ------------------------------------------------------------------------------
    @staticmethod
    async def _sleep(stop: asyncio.Event, seconds: float) -> None:
        try:
            await asyncio.wait_for(stop.wait(), timeout=seconds)
        except TimeoutError:
            pass

    async def _guarded(self, name: str, factory: Callable[[], Awaitable[None]], stop: asyncio.Event) -> None:
        """Run a long-lived task; a crash is recorded and the task restarted (never a silent dead loop)."""
        while not stop.is_set():
            try:
                await factory()
                return
            except asyncio.CancelledError:
                raise
            except Exception as exc:  # noqa: BLE001
                self.emit("TASK_CRASH", task=name, error=type(exc).__name__, detail=str(exc)[:200])
                self._log("task %s crashed: %s: %s" % (name, type(exc).__name__, exc))
                await self._sleep(stop, 5.0)

    # -- periodic tasks -------------------------------------------------------------------------
    async def _heartbeat(self, stop: asyncio.Event) -> None:
        interval = 1.0
        last = time.monotonic()
        last_emit = -1e9
        while not stop.is_set():
            await self._sleep(stop, interval)
            now = time.monotonic()
            self.loop_hb = now
            lag = now - last - interval
            last = now
            if lag > self.max_lag_s:
                self.max_lag_s = lag
            if lag > 1.0 and now - last_emit >= 30.0:
                last_emit = now
                self.emit("LOOP_LAG", lag_s=round(lag, 3))
            if not self.writer.is_alive() and not stop.is_set():
                self._log("writer thread died; exiting for a restart")
                self.exit_code = EXIT_WRITER_DEAD
                stop.set()

    async def _time_probes(self, stop: asyncio.Event) -> None:
        venues = sorted({c.venue for c in self.cfg.connections})
        interval = self.cfg.time_probe_interval_s
        await self._sleep(stop, min(10.0, interval))
        while not stop.is_set():
            for venue in venues:
                await self._probe_time(venue)
            await self._sleep(stop, interval)

    async def _probe_time(self, venue: str) -> None:
        if not self.governor.permit(venue, 1).ok:
            return  # deep sleep / budget / pressure: clock probes are the first thing to give up
        self.governor.spent(venue, 1)
        try:
            res = await self._fetch(venue, "time", {})
        except RestError as exc:
            self.emit("REST_FAIL", venue=venue, endpoint="time", reason=exc.kind)
            return
        self.governor.on_response(venue, res.status, res.headers)
        try:
            server_ms = int(json.loads(res.body)["serverTime"]) if res.status == 200 else None
        except (ValueError, KeyError, TypeError):
            server_ms = None
        if server_ms is None:
            self.emit("REST_FAIL", venue=venue, endpoint="time", status=res.status, reason="bad_response")
            return
        rtt_us = res.end_mono_us - res.start_mono_us
        mid_wall_us = res.start_wall_us + rtt_us // 2
        self.emit(
            "CLOCK_PROBE",
            venue=venue,
            server_ms=server_ms,
            rtt_us=rtt_us,
            offset_ms=round(server_ms - mid_wall_us / 1000.0, 1),
            start_wall_us=res.start_wall_us,
        )

    def _ref_targets(self) -> list[tuple[str, dict[str, object], str]]:
        targets: list[tuple[str, dict[str, object], str]] = []
        venues = {c.venue for c in self.cfg.connections}
        if "spot" in venues:
            targets += [("spot", {"symbol": s}, "spot:exchangeInfo:%s" % s) for s in self.cfg.symbols]
        if "usdm" in venues:
            targets.append(("usdm", {}, "usdm:exchangeInfo"))
        return targets

    async def _refdata(self, stop: asyncio.Event) -> None:
        """Reference data (exchange filters, tick sizes): at start, then daily, stored as ``ref`` records."""
        targets = self._ref_targets()
        due = {t[2]: 0.0 for t in targets}
        await self._sleep(stop, self._exchange_info_delay_s)
        while not stop.is_set():
            now = time.monotonic()
            for venue, params, stream in targets:
                if due[stream] > now or stop.is_set():
                    continue
                due[stream] = now + await self._capture_ref(venue, params, stream)
            await self._sleep(stop, 5.0)

    async def _capture_ref(self, venue: str, params: dict[str, object], stream: str) -> float:
        """Returns the delay until this reference item is due again."""
        weight = endpoint_weight(venue, "exchangeInfo", params)
        permit = self.governor.permit(venue, weight)
        if not permit.ok:
            return max(30.0, permit.retry_after_s)
        self.governor.spent(venue, weight)
        try:
            res = await self._fetch(venue, "exchangeInfo", params)
        except RestError as exc:
            self.emit("REST_FAIL", venue=venue, endpoint="exchangeInfo", reason=exc.kind)
            return REF_RETRY_S
        self.governor.on_response(venue, res.status, res.headers)
        if res.status != 200:
            self.emit("REST_FAIL", venue=venue, endpoint="exchangeInfo", status=res.status, reason="http")
            return REF_RETRY_S
        seq = self._submit_body(venue, "ref", stream, 0, res.body)
        self.emit(
            "REFDATA", venue=venue, stream=stream, bytes=len(res.body), rec_q=seq, accepted=seq is not None
        )
        return self.cfg.exchange_info_interval_s if seq is not None else REF_RETRY_S

    # -- main ---------------------------------------------------------------------------------
    async def run(self, stop: asyncio.Event) -> int:
        cfg = self.cfg
        self._stop = stop
        self._boot()
        loop = asyncio.get_running_loop()
        self._rest = RestClient(clock=self.clock, conn_factory=self._rest_conn_factory)
        for venue in {c.venue for c in cfg.connections}:
            self._rest_locks[venue] = asyncio.Lock()
        self.governor = RestGovernor(
            self.clock,
            self.state,
            self.emit,
            budget_fraction=cfg.rest_budget_fraction,
            pressure_fraction=cfg.rest_pressure_fraction,
        )
        env_kw: dict[str, Any] = {"rotation_age_s": cfg.rotation_age_s, "rng": self._rng}
        if self._connect is not None:
            env_kw["connect"] = self._connect
        env_kw.update(self._session_overrides)
        env = SessionEnv(
            clock=self.clock,
            seq=self.seq,
            run_no=self.run_no,
            writer=self.writer,
            emit=self.emit,
            governor=self.governor,
            stop=stop,
            **env_kw,
        )
        for spec in cfg.connections:
            self.managers[spec.name] = ConnectionManager(spec, env)
        for spec in cfg.connections:
            if not spec.is_depth:
                continue
            stream = spec.streams[0]
            assert stream.symbol is not None
            venue, symbol = spec.venue, stream.symbol
            mgr = self.managers[spec.name]
            tracker = DepthTracker(
                venue,
                symbol,
                spec.stream_id(stream.name),
                self.emit,
                on_first_event=lambda gen, v=venue, s=symbol: self.scheduler.first_event(v, s, gen),
                on_bridged=lambda gen, v=venue, s=symbol: self.scheduler.bridged(v, s, gen),
                on_gap=lambda rule, m=mgr: m.request_reconnect("gap:%s" % rule),
            )
            self.trackers[spec.stream_id(stream.name)] = tracker
            self._tracker_by_target[(venue, symbol)] = tracker
        env.trackers = self.trackers
        self.scheduler = SnapshotScheduler(
            clock=self.clock,
            governor=self.governor,
            fetch=self._fetch,
            sink=self._sink_snapshot,
            tracker_for=lambda v, s: self._tracker_by_target[(v, s)],
            emit=self.emit,
            targets=cfg.depth_targets,
            limit=cfg.snapshot_limit,
            cooldown_s=cfg.snapshot_cooldown_s,
            keyframe_interval_s=cfg.keyframe_interval_s,
        )
        env.scheduler = self.scheduler
        self._log(
            "run %d started: %d connections, symbols=%s"
            % (self.run_no, len(self.managers), ",".join(cfg.symbols))
        )

        conn_tasks = [loop.create_task(m.run(), name="conn:" + n) for n, m in self.managers.items()]
        aux_tasks = [
            loop.create_task(
                self._guarded("heartbeat", lambda: self._heartbeat(stop), stop), name="heartbeat"
            ),
            loop.create_task(self._guarded("snapshots", self.scheduler.run, stop), name="snapshots"),
            loop.create_task(
                self._guarded("time_probes", lambda: self._time_probes(stop), stop), name="time_probes"
            ),
            loop.create_task(self._guarded("refdata", lambda: self._refdata(stop), stop), name="refdata"),
        ]
        try:
            await stop.wait()
        finally:
            await self._shutdown(stop, conn_tasks, aux_tasks)
        return self.exit_code

    async def _shutdown(
        self, stop: asyncio.Event, conn_tasks: list[asyncio.Task[None]], aux_tasks: list[asyncio.Task[None]]
    ) -> None:
        stop.set()
        if conn_tasks:
            _, pending = await asyncio.wait(conn_tasks, timeout=CONN_STOP_TIMEOUT_S)
            for task in pending:
                task.cancel()
            await asyncio.gather(*conn_tasks, return_exceptions=True)
        await self.scheduler.stop()
        for task in aux_tasks:
            task.cancel()
        await asyncio.gather(*aux_tasks, return_exceptions=True)
        self._rest.close()
        self.writer.request_stop()
        await asyncio.to_thread(self.writer.join, WRITER_JOIN_TIMEOUT_S)
        if self._enable_monitor:
            self.monitor.stop()
        self._log(
            "run %d stopped (writer %s)"
            % (self.run_no, "done" if not self.writer.is_alive() else "STILL RUNNING")
        )


def run_forever(cfg: RecorderConfig) -> int:
    """Blocking entry point: run until SIGTERM/SIGINT, then shut down gracefully."""
    faulthandler.enable()

    async def amain() -> int:
        stop = asyncio.Event()
        loop = asyncio.get_running_loop()
        for sig in (signal.SIGTERM, signal.SIGINT):
            try:
                loop.add_signal_handler(sig, stop.set)
            except NotImplementedError:  # Windows: fall back to the plain handler
                signal.signal(sig, lambda *_a: loop.call_soon_threadsafe(stop.set))
        return await Recorder(cfg).run(stop)

    return asyncio.run(amain())
