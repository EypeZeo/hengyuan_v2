#!/usr/bin/env python3
"""Load and latency measurement of the whole recorder pipeline (D0-2). Not collected by pytest, not run by CI.

    python tests/loadgen.py --mult 3 --seconds 60 --level 9 --root /tmp/hy-load

Runs the real ``Recorder`` (real websockets client, real receive path, real writer thread, real files)
against a synthetic exchange in the same process that sends frames of realistic size at ``mult`` times
the rates measured on 2026-09-29 (DATA_DICTIONARY.md section 8). The synthetic exchange runs in this
process too, so CPU figures are an UPPER bound for the recorder itself (its busy time is reported
separately so an estimate can be derived). Prices and quantities are random, not real market data, so the
compression ratio is a lower bound of what real, more correlated data achieves.

It measures performance, not correctness (tests/test_recorder_e2e.py covers correctness): snapshots are
answered with errors, so depth streams stay unbridged.
"""

from __future__ import annotations

import argparse
import asyncio
import gc
import json
import os
import random
import shutil
import sys
import time
from array import array
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from websockets.asyncio.client import connect as ws_connect  # noqa: E402
from websockets.asyncio.server import serve  # noqa: E402
from websockets.protocol import State  # noqa: E402

from hy_recorder.config import default_config  # noqa: E402
from hy_recorder.manifest import Manifest  # noqa: E402
from hy_recorder.recorder import Recorder  # noqa: E402
from hy_recorder.segment import SegmentWriter  # noqa: E402
from hy_recorder.session import ConnectionManager  # noqa: E402
from hy_recorder.writer import Writer  # noqa: E402

# frames per second measured at the recording host (DATA_DICTIONARY.md section 8)
RATES = {"spot_depth": 10.0, "spot_trade": 35.7, "usdm_depth": 9.8, "agg": 15.0, "mark": 1.0, "force": 0.4}
LOCAL = {
    "spot_depth_btcusdt": "/spot_depth",
    "usdm_depth_btcusdt": "/usdm_depth",
    "spot_trade": "/spot_trade",
    "usdm_market": "/usdm_market",
}


class Synth:
    """Frames of realistic size and shape with random content."""

    def __init__(self, seed: int = 1) -> None:
        self.rng = random.Random(seed)
        self.pool = [
            '["%.2f","%.3f"]' % (84000 + self.rng.uniform(-3000, 3000), self.rng.uniform(0.001, 40))
            for _ in range(4000)
        ]
        self.spot_u = self.usdm_u = self.trade_id = self.agg_id = 0
        self.mark_e = int(time.time() * 1000)

    def _levels(self, n: int) -> str:
        i = self.rng.randrange(len(self.pool) - n)
        return ",".join(self.pool[i : i + n])

    def spot_depth(self) -> str:
        lo, self.spot_u = self.spot_u + 1, self.spot_u + 1 + self.rng.randrange(1, 4)
        now = int(time.time() * 1000)
        return (
            '{"stream":"btcusdt@depth@100ms","data":{"e":"depthUpdate","E":%d,"s":"BTCUSDT","U":%d,"u":%d,"b":[%s],"a":[%s]}}'
            % (now, lo, self.spot_u, self._levels(24), self._levels(24))
        )

    def usdm_depth(self) -> str:
        lo, pu = self.usdm_u + 1, self.usdm_u
        self.usdm_u = lo + self.rng.randrange(2, 20)
        now = int(time.time() * 1000)
        return (
            '{"stream":"btcusdt@depth@100ms","data":{"e":"depthUpdate","E":%d,"T":%d,"s":"BTCUSDT","ps":"BTCUSDT","U":%d,"u":%d,"pu":%d,"b":[%s],"a":[%s]}}'
            % (now, now - 1, lo, self.usdm_u, pu, self._levels(64), self._levels(64))
        )

    def trade(self) -> str:
        self.trade_id += 1
        now = int(time.time() * 1000)
        return (
            '{"stream":"btcusdt@trade","data":{"e":"trade","E":%d,"s":"BTCUSDT","t":%d,"p":"%.8f","q":"%.8f","T":%d,"m":true,"M":true}}'
            % (now, self.trade_id, 84000 + self.rng.uniform(-50, 50), self.rng.uniform(0.0001, 2), now)
        )

    def agg(self) -> str:
        self.agg_id += 1
        now = int(time.time() * 1000)
        return (
            '{"stream":"btcusdt@aggTrade","data":{"e":"aggTrade","E":%d,"a":%d,"s":"BTCUSDT","p":"%.1f","q":"%.3f","f":%d,"l":%d,"T":%d,"m":false}}'
            % (
                now,
                self.agg_id,
                84000 + self.rng.uniform(-50, 50),
                self.rng.uniform(0.001, 3),
                self.agg_id * 3,
                self.agg_id * 3 + 2,
                now,
            )
        )

    def mark(self) -> str:
        self.mark_e += 1000
        return (
            '{"stream":"btcusdt@markPrice@1s","data":{"e":"markPriceUpdate","E":%d,"s":"BTCUSDT","p":"%.8f","P":"%.8f","i":"%.8f","r":"0.00010000","T":%d}}'
            % (self.mark_e, 84000 + self.rng.uniform(-9, 9), 84010.0, 84001.0, self.mark_e + 1000)
        )

    def force(self) -> str:
        now = int(time.time() * 1000)
        return (
            '{"stream":"!forceOrder@arr","data":{"e":"forceOrder","E":%d,"o":{"s":"BTCUSDT","S":"SELL","o":"LIMIT","f":"IOC","q":"0.014","p":"9910","ap":"9910","X":"FILLED","l":"0.014","z":"0.014","T":%d}}}'
            % (now, now)
        )


async def pace(ws, gens, mult: float, deadline: float, busy_ns: list[int]) -> None:
    now = time.monotonic()
    due = [now + random.random() / (r * mult) for r, _ in gens]
    while ws.state is State.OPEN and time.monotonic() < deadline:
        i = min(range(len(gens)), key=due.__getitem__)
        delay = due[i] - time.monotonic()
        if delay > 0:
            await asyncio.sleep(delay)
        t0 = time.perf_counter_ns()
        await ws.send(gens[i][1]())
        busy_ns[0] += time.perf_counter_ns() - t0
        due[i] += 1.0 / (gens[i][0] * mult)


def pct(a: array, p: float) -> float:
    if not a:
        return 0.0
    s = sorted(a)
    return float(s[min(len(s) - 1, int(p / 100.0 * len(s)))])


def thread_cpu() -> dict[str, float]:
    """CPU seconds per thread name (Linux only)."""
    out: dict[str, float] = {}
    task = Path("/proc/self/task")
    if not task.exists():
        return out
    tick = os.sysconf("SC_CLK_TCK")
    for d in task.iterdir():
        try:
            name = (d / "comm").read_text().strip()
            fields = (d / "stat").read_text().rsplit(")", 1)[1].split()
            out[name] = out.get(name, 0.0) + (int(fields[11]) + int(fields[12])) / tick
        except (OSError, IndexError, ValueError):
            continue
    return out


def psi() -> dict[str, str]:
    out = {}
    for kind in ("cpu", "memory", "io"):
        try:
            out[kind] = Path("/proc/pressure/" + kind).read_text().splitlines()[0]
        except OSError:
            pass
    return out


def vm_hwm_mb() -> float | None:
    try:
        for line in Path("/proc/self/status").read_text().splitlines():
            if line.startswith("VmHWM:"):
                return int(line.split()[1]) / 1024
    except OSError:
        pass
    return None


class FailingConn:
    def request(self, *a, **k):
        raise OSError("load test: REST disabled")

    def getresponse(self):  # pragma: no cover
        raise OSError

    def close(self):
        pass


async def run(args) -> dict:
    root = Path(args.root)
    if root.exists():
        shutil.rmtree(root)
    cfg = default_config(root, zstd_level=args.level, queue_capacity=args.queue)
    synth = Synth()
    gens = {
        "/spot_depth": [(RATES["spot_depth"], synth.spot_depth)],
        "/usdm_depth": [(RATES["usdm_depth"], synth.usdm_depth)],
        "/spot_trade": [(RATES["spot_trade"], synth.trade)],
        "/usdm_market": [
            (RATES["agg"], synth.agg),
            (RATES["mark"], synth.mark),
            (RATES["force"], synth.force),
        ],
    }
    srv_busy = [0]
    deadline = time.monotonic() + args.seconds + 5

    async def handler(ws):
        path = ws.request.path.split("?")[0]
        if path in gens:
            await pace(ws, gens[path], args.mult, deadline, srv_busy)

    server = await serve(handler, "127.0.0.1", 0)
    port = server.sockets[0].getsockname()[1]
    by_url = {c.url: LOCAL[c.name] for c in cfg.connections}

    def connect(url, **kw):
        return ws_connect("ws://127.0.0.1:%d%s" % (port, by_url[url]), **kw)

    # instrumentation
    rx_ns, lat_us, wr_ns, fl_ns, lag_us = array("q"), array("q"), array("q"), array("q"), array("q")
    raw_bytes = [0]
    lag_events: list[tuple[float, float]] = []  # (seconds since the probe started, lag in ms) for lag > 30 ms
    gc_pauses: dict[int, list[float]] = {0: [], 1: [], 2: []}
    gc_mark = [0]

    def gc_cb(phase, info):
        if phase == "start":
            gc_mark[0] = time.perf_counter_ns()
        else:
            gc_pauses[info["generation"]].append((time.perf_counter_ns() - gc_mark[0]) / 1e6)

    gc.callbacks.append(gc_cb)
    orig_msg, orig_one = ConnectionManager._on_message, Writer._write_one
    orig_write, orig_flush = SegmentWriter.write, SegmentWriter.flush_block

    def t_msg(self, msg, gen):
        t0 = time.perf_counter_ns()
        orig_msg(self, msg, gen)
        rx_ns.append(time.perf_counter_ns() - t0)

    def t_one(self, key, item):
        orig_one(self, key, item)
        lat_us.append(time.time_ns() // 1000 - item[2])

    def t_write(self, line, **kw):
        t0 = time.perf_counter_ns()
        try:
            return orig_write(self, line, **kw)
        finally:
            wr_ns.append(time.perf_counter_ns() - t0)
            raw_bytes[0] += len(line)

    def t_flush(self):
        t0 = time.perf_counter_ns()
        try:
            return orig_flush(self)
        finally:
            fl_ns.append(time.perf_counter_ns() - t0)

    ConnectionManager._on_message, Writer._write_one = t_msg, t_one
    SegmentWriter.write, SegmentWriter.flush_block = t_write, t_flush
    try:
        rec = Recorder(
            cfg,
            connect=connect,
            rest_conn_factory=lambda host: FailingConn(),
            log=lambda m: None,
            enable_monitor=False,
            exchange_info_delay_s=10_000,
            session_overrides={"measure_dns": False, "idle_poll_s": 0.2},
        )
        stop = asyncio.Event()

        probe_start = time.perf_counter()

        async def lag_probe():
            while not stop.is_set():
                t0 = time.perf_counter()
                await asyncio.sleep(0.05)
                lag = time.perf_counter() - t0 - 0.05
                lag_us.append(int(lag * 1e6))
                if lag > 0.03:
                    lag_events.append((round(t0 - probe_start, 2), round(lag * 1000, 1)))

        psi0, cpu0, thr0, wall0 = psi(), time.process_time(), thread_cpu(), time.monotonic()
        probe = asyncio.create_task(lag_probe())
        task = asyncio.create_task(rec.run(stop))
        await asyncio.sleep(args.seconds)
        cpu1, thr1, wall1, psi1 = time.process_time(), thread_cpu(), time.monotonic(), psi()
        stop.set()
        await asyncio.wait_for(task, 120)
        await probe
    finally:
        gc.callbacks.remove(gc_cb)
        ConnectionManager._on_message, Writer._write_one = orig_msg, orig_one
        SegmentWriter.write, SegmentWriter.flush_block = orig_write, orig_flush
    server.close()
    await asyncio.wait_for(server.wait_closed(), 10)

    status = json.loads((root / "status.json").read_bytes())
    sealed = sum(int(e.get("bytes") or 0) for e in Manifest(root).live_segments().values())
    dur = wall1 - wall0
    frames = sum(q["written"] for q in status["queues"].values())
    thr = {n: round(thr1.get(n, 0.0) - thr0.get(n, 0.0), 2) for n in thr1}
    return {
        "mult": args.mult,
        "level": args.level,
        "seconds": round(dur, 1),
        "frames_written": frames,
        "frames_per_s": round(frames / dur, 1),
        "dropped": {k: v["dropped"] for k, v in status["queues"].items() if v["dropped"]},
        "queue_high_water": {k: v["high_water"] for k, v in status["queues"].items()},
        "write_errors": status["write_errors"],
        "receive_path_us": {
            "p50": pct(rx_ns, 50) / 1e3,
            "p99": pct(rx_ns, 99) / 1e3,
            "p99.9": pct(rx_ns, 99.9) / 1e3,
            "max": max(rx_ns, default=0) / 1e3,
        },
        "enqueue_to_written_ms": {
            "p50": pct(lat_us, 50) / 1e3,
            "p99": pct(lat_us, 99) / 1e3,
            "p99.9": pct(lat_us, 99.9) / 1e3,
            "max": max(lat_us, default=0) / 1e3,
        },
        "segment_write_us": {
            "p50": pct(wr_ns, 50) / 1e3,
            "p99": pct(wr_ns, 99) / 1e3,
            "p99.9": pct(wr_ns, 99.9) / 1e3,
        },
        "block_flush_ms": {
            "p50": pct(fl_ns, 50) / 1e6,
            "p99": pct(fl_ns, 99) / 1e6,
            "max": max(fl_ns, default=0) / 1e6,
        },
        "loop_lag_ms": {
            "p50": pct(lag_us, 50) / 1e3,
            "p99": pct(lag_us, 99) / 1e3,
            "p99.9": pct(lag_us, 99.9) / 1e3,
            "max": max(lag_us, default=0) / 1e3,
        },
        "loop_lag_events_over_30ms": lag_events[:20],
        "gc_pauses_ms": {
            f"gen{g}": {"n": len(v), "max": round(max(v, default=0), 1)} for g, v in gc_pauses.items()
        },
        "cpu_pct_of_one_core": round(100 * (cpu1 - cpu0) / dur, 1),
        "cpu_seconds_by_thread": thr,
        "synthetic_exchange_busy_s": round(srv_busy[0] / 1e9, 2),
        "vm_hwm_mb": vm_hwm_mb(),
        "raw_mb": round(raw_bytes[0] / 1e6, 1),
        "sealed_mb": round(sealed / 1e6, 1),
        "compression_ratio": round(raw_bytes[0] / sealed, 2) if sealed else None,
        "compressed_mb_per_day_at_1x": round(sealed / 1e6 / (dur * args.mult) * 86400) if sealed else None,
        "psi_before": psi0,
        "psi_after": psi1,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mult", type=float, default=3.0)
    ap.add_argument("--seconds", type=float, default=60.0)
    ap.add_argument("--level", type=int, default=9)
    ap.add_argument("--queue", type=int, default=20_000)
    ap.add_argument("--root", default=str(Path(os.environ.get("TMPDIR", "/tmp")) / "hy-load"))
    args = ap.parse_args()
    result = asyncio.run(run(args))
    print(json.dumps(result, indent=1))
    return 0 if not result["dropped"] and result["write_errors"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
