"""The single writer thread: owns compression, segment files, manifest, ledger and state.

The event loop thread (receiver) only stamps time, draws a sequence number and calls
:meth:`Writer.submit_record`, which is ``put_nowait`` on a bounded queue: it never blocks and never
waits for disk or CPU. When a queue is full the record is dropped on the floor **visibly**: its
sequence number is reported in a coalesced ``OVERRUN`` ledger event, so the offline verifier can
match the resulting hole to an explanation instead of silently accepting it.

Disk protection lives here too: a periodic free-space check, the rolling cleanup
(:mod:`hy_recorder.retention`), an emergency reserve file, and a hard stop of writing (records
counted as overrun, ``DISK_STOP`` in the ledger) only when cleanup cannot restore the floor.
"""

from __future__ import annotations

import errno
import json
import queue
import shutil
import threading
import time
from collections.abc import Callable
from pathlib import Path
from typing import Any

from .clock import Clock
from .config import RecorderConfig
from .fsutil import atomic_write_json
from .ledger import Ledger, make_event
from .manifest import Manifest
from .retention import ensure_reserve, release_reserve, run_cleanup
from .segment import SegmentWriter
from .seq import SeqCounter
from .state import StateFile

GB = 1 << 30
FLUSH_INTERVAL_S = 1.0
SYNC_INTERVAL_S = 2.0
STATUS_INTERVAL_S = 10.0
DISK_INTERVAL_S = 60.0
RETENTION_INTERVAL_S = 300.0
OVERRUN_EMIT_INTERVAL_S = 1.0
MAX_OVERRUN_RANGES = 50
CONTROL_CAP = 100_000
_RANGE_GAP = 4  # dropped sequence numbers this close together are merged into one range


class Writer(threading.Thread):
    def __init__(
        self,
        cfg: RecorderConfig,
        *,
        state: StateFile,
        manifest: Manifest,
        ledger: Ledger,
        clock: Clock,
        seq: SeqCounter,
        run_no: int,
        status_extra: Callable[[], dict[str, Any]] | None = None,
    ) -> None:
        super().__init__(name="hy-writer", daemon=False)
        self.cfg = cfg
        self.state, self.manifest, self.ledger, self.clock, self.seq, self.run_no = (
            state,
            manifest,
            ledger,
            clock,
            seq,
            run_no,
        )
        self._status_extra = status_extra
        classes = sorted(
            {(c.venue, c.cls) for c in cfg.connections}
            | {(v, "snapshot") for v, _ in cfg.depth_targets}
            | {(c.venue, "ref") for c in cfg.connections}
        )  # "ref": reference data (exchange filters)
        self._queues: dict[tuple[str, str], queue.Queue[tuple[bytes, int, int, str]]] = {
            k: queue.Queue(maxsize=cfg.queue_capacity) for k in classes
        }
        self._high_water = {k: 0 for k in classes}
        self._control: queue.SimpleQueue[dict[str, Any]] = queue.SimpleQueue()
        self._control_n = 0
        self._segments: dict[tuple[str, str], SegmentWriter] = {}
        self._overrun_lock = threading.Lock()
        self._overrun: dict[tuple[str, str], list[list[int]]] = {}
        self._dropped_total = {k: 0 for k in classes}
        self._written_total = {k: 0 for k in classes}
        self._stop_evt = threading.Event()
        self.hb_mono = time.monotonic()
        self.disk_stopped = False
        self.write_errors = 0
        self._last_status: dict[str, Any] = {}
        self._warned_at = -1e9

    # -- producer side (event loop thread; never blocks) ------------------------------------
    def submit_record(
        self, venue: str, cls: str, line: bytes, *, seq: int, wall_us: int, stream: str
    ) -> bool:
        key = (venue, cls)
        if self.disk_stopped or self._stop_evt.is_set():
            self._note_drop(key, seq)
            return False
        q = self._queues[key]
        try:
            q.put_nowait((line, seq, wall_us, stream))
        except queue.Full:
            self._note_drop(key, seq)
            return False
        depth = q.qsize()
        if depth > self._high_water[key]:
            self._high_water[key] = depth
        return True

    def submit_event(self, event: dict[str, Any]) -> None:
        """Queue a ledger event (already carrying run/q/t/m/k). Bounded by CONTROL_CAP."""
        if self._control_n >= CONTROL_CAP:
            return
        self._control_n += 1
        self._control.put(event)

    def _note_drop(self, key: tuple[str, str], seq: int) -> None:
        with self._overrun_lock:
            self._dropped_total[key] += 1
            ranges = self._overrun.setdefault(key, [])
            if ranges and seq - ranges[-1][1] <= _RANGE_GAP:
                ranges[-1][1] = seq
                ranges[-1][2] += 1
            else:
                ranges.append([seq, seq, 1])  # [first_q, last_q, count]

    def request_stop(self) -> None:
        self._stop_evt.set()

    # -- consumer side (this thread) ---------------------------------------------------------
    def _segment(self, key: tuple[str, str]) -> SegmentWriter:
        w = self._segments.get(key)
        if w is None:
            w = self._segments[key] = SegmentWriter(
                self.cfg.root,
                key[0],
                key[1],
                level=self.cfg.zstd_level,
                next_segseq=self.state.next_segseq,
                manifest=self.manifest,
                clock=self.clock,
            )
        return w

    def _write_one(self, key: tuple[str, str], item: tuple[bytes, int, int, str]) -> None:
        line, seq, wall_us, stream = item
        try:
            self._segment(key).write(line, run=self.run_no, seq=seq, wall_us=wall_us, stream=stream)
            self._written_total[key] += 1
        except OSError as exc:
            self._handle_io_error(exc, key, seq)

    def _handle_io_error(self, exc: OSError, key: tuple[str, str], seq: int) -> None:
        self.write_errors += 1
        self._note_drop(key, seq)
        self._ledger("WRITE_ERROR", errno=exc.errno, error=type(exc).__name__, cls="%s/%s" % key)
        if exc.errno in (errno.ENOSPC, errno.EDQUOT):
            freed = release_reserve(self.cfg.root)
            if freed:
                self._ledger("RESERVE_RELEASED", bytes=freed, cause="write_enospc")
            self.disk_stopped = True
            self._ledger("DISK_STOP", cause="write_enospc", free_bytes=self._free())

    def _ledger(self, kind: str, **fields: Any) -> None:
        ev = make_event(
            self.run_no, self.seq.next(), self.clock.wall_us(), self.clock.mono_us(), kind, **fields
        )
        try:
            self.ledger.append(ev, force_sync=kind in ("PROC_STOP", "DISK_STOP", "WRITE_ERROR"))
        except OSError:
            pass  # a full disk must not kill the writer; the failure is visible in status.json

    def _free(self) -> int:
        try:
            return shutil.disk_usage(self.cfg.root).free
        except OSError:
            return 0

    def _drain_control(self) -> None:
        for _ in range(1000):
            try:
                ev = self._control.get_nowait()
            except queue.Empty:
                return
            self._control_n = max(0, self._control_n - 1)
            try:
                self.ledger.append(
                    ev, force_sync=ev.get("k") in ("PROC_START", "PROC_STOP", "GAP_DETECTED", "BAN")
                )
            except OSError:
                self.write_errors += 1

    def _emit_overruns(self, force: bool = False) -> None:
        with self._overrun_lock:
            pending, self._overrun = self._overrun, {}
        for key, ranges in pending.items():
            if len(ranges) > MAX_OVERRUN_RANGES:  # bound the ledger: merge the tail into one wide range
                head, tail = ranges[: MAX_OVERRUN_RANGES - 1], ranges[MAX_OVERRUN_RANGES - 1 :]
                ranges = head + [[tail[0][0], tail[-1][1], sum(r[2] for r in tail)]]
            for first_q, last_q, count in ranges:
                self._ledger(
                    "OVERRUN",
                    cls="%s/%s" % key,
                    venue=key[0],
                    klass=key[1],
                    dropped=count,
                    first_q=first_q,
                    last_q=last_q,
                    disk_stopped=self.disk_stopped,
                )

    def _disk_check(self) -> None:
        free = self._free()
        warn, floor = int(self.cfg.disk_warn_gb * GB), int(self.cfg.disk_floor_gb * GB)
        if free < warn:
            if time.monotonic() - self._warned_at > 600:
                self._warned_at = time.monotonic()
                self._ledger("DISK_WARN", free_bytes=free, warn_bytes=warn)
            self._retention()
            free = self._free()
        if free < floor and not self.disk_stopped:
            freed = release_reserve(self.cfg.root)
            if freed:
                self._ledger("RESERVE_RELEASED", bytes=freed, cause="below_floor")
            self.disk_stopped = True
            self._ledger("DISK_STOP", cause="below_floor", free_bytes=free, floor_bytes=floor)
        elif self.disk_stopped and free >= warn:
            self.disk_stopped = False
            self._ledger("DISK_RESUMED", free_bytes=free)
        if not self.disk_stopped:
            try:
                ensure_reserve(self.cfg.root, self.cfg.reserve_mb << 20, free, warn)
            except OSError:
                pass

    def _retention(self) -> None:
        try:
            plan = run_cleanup(
                self.cfg.root,
                self.manifest,
                self.clock,
                self._ledger,
                warn_bytes=int(self.cfg.disk_warn_gb * GB),
                floor_bytes=int(self.cfg.disk_floor_gb * GB),
                retain_hours=self.cfg.retain_hours,
            )
            if plan.hard_stop and not self.disk_stopped:
                self._ledger("RETENTION_EXHAUSTED", free_after=plan.free_after)
        except OSError as exc:
            self.write_errors += 1
            self._ledger("RETENTION_FAILED", error=type(exc).__name__)

    def _status(self) -> dict[str, Any]:
        now_us = self.clock.wall_us()
        live = self.manifest.live_segments()
        day_ago = now_us - 24 * 3_600_000_000
        grown = sum(
            int(e.get("bytes") or 0)
            for e in live.values()
            if int((e.get("last") or {}).get("t") or 0) >= day_ago
        )
        free = self._free()
        floor = int(self.cfg.disk_floor_gb * GB)
        runway_h = round((free - floor) / (grown / 24.0), 1) if grown > 0 and free > floor else None
        st: dict[str, Any] = {
            "run": self.run_no,
            "now_us": now_us,
            "last_q": self.seq.last,
            "queues": {
                "%s/%s" % k: {
                    "depth": q.qsize(),
                    "high_water": self._high_water[k],
                    "written": self._written_total[k],
                    "dropped": self._dropped_total[k],
                }
                for k, q in self._queues.items()
            },
            "disk": {
                "free_bytes": free,
                "sealed_bytes_24h": grown,
                "runway_hours": runway_h,
                "stopped": self.disk_stopped,
                "warn_gb": self.cfg.disk_warn_gb,
                "floor_gb": self.cfg.disk_floor_gb,
            },
            "write_errors": self.write_errors,
            "segments_live": len(live),
        }
        if self._status_extra is not None:
            try:
                st.update(self._status_extra())
            except Exception as exc:  # noqa: BLE001 - status must never take the writer down
                st["status_extra_error"] = type(exc).__name__
        return st

    def _write_status(self) -> None:
        self._last_status = self._status()
        try:
            atomic_write_json(Path(self.cfg.root) / "status.json", self._last_status)
        except OSError:
            self.write_errors += 1

    def status_snapshot(self) -> dict[str, Any]:
        return self._last_status

    def run(self) -> None:
        last_flush = last_sync = last_status = last_disk = last_ret = last_overrun = time.monotonic()
        try:
            ensure_reserve(
                self.cfg.root, self.cfg.reserve_mb << 20, self._free(), int(self.cfg.disk_warn_gb * GB)
            )
        except OSError:
            pass
        while True:
            drained = 0
            for key, q in self._queues.items():
                for _ in range(512):
                    try:
                        item = q.get_nowait()
                    except queue.Empty:
                        break
                    self._write_one(key, item)
                    drained += 1
            self._drain_control()
            now = time.monotonic()
            if now - last_overrun >= OVERRUN_EMIT_INTERVAL_S:
                self._emit_overruns()
                last_overrun = now
            if now - last_flush >= FLUSH_INTERVAL_S:
                for w in self._segments.values():
                    try:
                        w.flush_block()
                    except OSError as exc:
                        self._handle_io_error(exc, ("-", "-"), 0)
                last_flush = now
            if now - last_sync >= SYNC_INTERVAL_S:
                for w in self._segments.values():
                    try:
                        w.sync()
                    except OSError as exc:
                        self._handle_io_error(exc, ("-", "-"), 0)
                try:
                    self.ledger.sync()
                except OSError:
                    self.write_errors += 1
                last_sync = now
            if now - last_disk >= DISK_INTERVAL_S:
                self._disk_check()
                last_disk = now
            if now - last_ret >= RETENTION_INTERVAL_S:
                self._retention()
                last_ret = now
            if now - last_status >= STATUS_INTERVAL_S:
                self._write_status()
                last_status = now
            self.hb_mono = now
            if (
                self._stop_evt.is_set()
                and drained == 0
                and all(q.empty() for q in self._queues.values())
                and self._control.empty()
            ):
                break
            if drained == 0:
                time.sleep(0.02)
        self._finish()

    def _finish(self) -> None:
        self._emit_overruns(force=True)
        self._drain_control()
        for w in self._segments.values():
            try:
                w.seal("shutdown")
            except OSError as exc:
                self.write_errors += 1
                self._ledger("WRITE_ERROR", errno=exc.errno, error=type(exc).__name__, cls="seal")
        self._ledger(
            "PROC_STOP",
            reason="shutdown",
            write_errors=self.write_errors,
            dropped={"%s/%s" % k: v for k, v in self._dropped_total.items() if v},
        )
        self.ledger.close()
        self._write_status()
        if self.write_errors == 0:
            self.state.mark_clean()

    def stop_and_join(self, timeout: float = 60.0) -> None:
        self.request_stop()
        self.join(timeout)


def dump_status(root: Path) -> str:
    """Human-readable status file content (for the ``status`` CLI command)."""
    return json.dumps(json.loads((Path(root) / "status.json").read_bytes()), indent=1, sort_keys=True)
