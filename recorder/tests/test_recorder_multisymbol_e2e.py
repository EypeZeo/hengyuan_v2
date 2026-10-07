"""D0-5a rehearsal: the whole recorder with TWO symbols against a local fake exchange.

D0-4 (the 24 h shadow run) was single-symbol, and so is ``test_recorder_e2e.py``. D0-5a adds ETHUSDT
to the symbol list. The connection layout is derived from that list (``config.build_connections``) and
unit-tested, but until now nothing ran two symbols end to end: two depth connections per venue writing
into one segment class, one snapshot and one bridge per (venue, symbol), one trade connection and one
market connection carrying both symbols, a planned rotation of all six connections at once, then the
offline verifier and the operations report over the result.

Real sockets, real writer thread, real files; no network and no host. Each symbol gets its own id space
(like the real exchange), so a frame routed to the wrong stream breaks that stream's id chain and the
verifier says so.
"""

from __future__ import annotations

import asyncio
import json
import time
from collections import Counter
from dataclasses import replace
from urllib.parse import parse_qs, urlsplit

import websockets
from websockets.asyncio.client import connect as ws_connect
from websockets.asyncio.server import serve
from websockets.protocol import State

from hy_recorder.clock import SYSTEM_CLOCK
from hy_recorder.config import default_config
from hy_recorder.ledger import Ledger
from hy_recorder.recorder import Recorder
from hy_recorder.report import build_report
from hy_recorder.verify import verify_lake

SYMBOLS = ("BTCUSDT", "ETHUSDT")
DEPTH_CONNS = {
    "spot_depth_btcusdt": ("spot", "BTCUSDT"),
    "usdm_depth_btcusdt": ("usdm", "BTCUSDT"),
    "spot_depth_ethusdt": ("spot", "ETHUSDT"),
    "usdm_depth_ethusdt": ("usdm", "ETHUSDT"),
}
CONNS = set(DEPTH_CONNS) | {"spot_trade", "usdm_market"}
ROTATION_AGE_S = 6.0


class MultiMarket:
    """Shared truth of the fake exchange: per-symbol monotone ids in disjoint ranges."""

    def __init__(self):
        self.spot_u = {s: 1_000 + i * 1_000_000 for i, s in enumerate(SYMBOLS)}
        self.usdm_u = {s: 5_000 + i * 1_000_000 for i, s in enumerate(SYMBOLS)}
        self.trade_id = {s: i * 1_000_000 for i, s in enumerate(SYMBOLS)}
        self.agg_id = {s: i * 1_000_000 for i, s in enumerate(SYMBOLS)}
        self.mark_e = {s: 1_790_000_000_000 + i * 17 for i, s in enumerate(SYMBOLS)}
        self.opened_paths: list[str] = []

    def spot_depth(self, sym):
        lo = self.spot_u[sym] + 1
        self.spot_u[sym] = lo + 1
        return {
            "e": "depthUpdate",
            "E": int(time.time() * 1000),
            "s": sym,
            "U": lo,
            "u": self.spot_u[sym],
            "b": [["1.0", "1"]],
            "a": [["2.0", "1"]],
        }

    def usdm_depth(self, sym):
        lo = self.usdm_u[sym] + 1
        pu = self.usdm_u[sym]
        self.usdm_u[sym] = lo + 2  # width 3: a snapshot at last_u-1 always falls inside the last event
        return {
            "e": "depthUpdate",
            "E": int(time.time() * 1000),
            "T": int(time.time() * 1000),
            "s": sym,
            "U": lo,
            "u": self.usdm_u[sym],
            "pu": pu,
            "b": [["1.0", "1"]],
            "a": [["2.0", "1"]],
        }

    def trade(self, sym):
        self.trade_id[sym] += 1
        now = int(time.time() * 1000)
        return {
            "e": "trade",
            "E": now,
            "s": sym,
            "t": self.trade_id[sym],
            "p": "100.0",
            "q": "0.1",
            "T": now,
            "m": True,
            "M": True,
        }

    def agg(self, sym):
        self.agg_id[sym] += 1
        now = int(time.time() * 1000)
        return {
            "e": "aggTrade",
            "E": now,
            "s": sym,
            "a": self.agg_id[sym],
            "p": "100.0",
            "q": "0.1",
            "f": self.agg_id[sym] * 10,
            "l": self.agg_id[sym] * 10,
            "T": now,
            "m": False,
        }

    def mark(self, sym):
        self.mark_e[sym] += 1000
        return {
            "e": "markPriceUpdate",
            "E": self.mark_e[sym],
            "s": sym,
            "p": "100.0",
            "i": "100.0",
            "P": "100.0",
            "r": "0.0001",
            "T": self.mark_e[sym] + 1000,
        }


def frame(stream, data):
    return json.dumps({"stream": stream, "data": data}, separators=(",", ":"))


async def fake_exchange(market: MultiMarket):
    """One local server; the request path is the connection name and says which fake streams to serve."""

    async def handler(ws):
        path = urlsplit(ws.request.path).path
        market.opened_paths.append(path)
        try:
            if path[1:] in DEPTH_CONNS:
                venue, sym = DEPTH_CONNS[path[1:]]
                make = market.spot_depth if venue == "spot" else market.usdm_depth
                while ws.state is State.OPEN:
                    await ws.send(frame("%s@depth@100ms" % sym.lower(), make(sym)))
                    await asyncio.sleep(0.004)
            elif path == "/spot_trade":
                while ws.state is State.OPEN:
                    for sym in SYMBOLS:
                        await ws.send(frame("%s@trade" % sym.lower(), market.trade(sym)))
                    await asyncio.sleep(0.005)
            elif path == "/usdm_market":
                n = 0
                while ws.state is State.OPEN:
                    for sym in SYMBOLS:
                        await ws.send(frame("%s@aggTrade" % sym.lower(), market.agg(sym)))
                    n += 1
                    if n % 10 == 0:
                        for sym in SYMBOLS:
                            await ws.send(frame("%s@markPrice@1s" % sym.lower(), market.mark(sym)))
                    await asyncio.sleep(0.005)
            else:
                await ws.close(1008)
        except websockets.ConnectionClosed:
            return

    server = await serve(handler, "127.0.0.1", 0)
    return server, server.sockets[0].getsockname()[1]


class FakeHttpResponse:
    def __init__(self, status, body, headers=None):
        self.status = status
        self._body = body
        self._headers = headers or {"content-type": "application/json", "x-mbx-used-weight-1m": "7"}
        self.will_close = False

    def read(self, n=-1):
        return self._body

    def getheaders(self):
        return list(self._headers.items())


class MultiRestConn:
    """Serves the allowlisted GETs from the fake market, per requested symbol."""

    def __init__(self, market: MultiMarket, requests: list[str]):
        self.market, self.requests, self._resp = market, requests, None

    def request(self, method, path, headers=None):
        assert method == "GET" and set(headers) <= {"User-Agent", "Accept"}
        self.requests.append(path)
        parts = urlsplit(path)
        p, sym = parts.path, (parse_qs(parts.query).get("symbol") or [""])[0].upper()
        m = self.market
        if p == "/api/v3/depth":
            body = {"lastUpdateId": m.spot_u[sym], "bids": [["1.0", "1"]], "asks": [["2.0", "1"]]}
        elif p == "/fapi/v1/depth":
            body = {
                "lastUpdateId": m.usdm_u[sym] - 1,
                "E": 1,
                "T": 1,
                "bids": [["1.0", "1"]],
                "asks": [["2.0", "1"]],
            }
        elif p in ("/api/v3/time", "/fapi/v1/time"):
            body = {"serverTime": int(time.time() * 1000) + 12}
        elif p == "/api/v3/exchangeInfo":
            body = {"timezone": "UTC", "symbols": [{"symbol": sym, "filters": []}]}
        elif p == "/fapi/v1/exchangeInfo":
            body = {
                "timezone": "UTC",
                "symbols": [{"symbol": s, "contractType": "PERPETUAL"} for s in SYMBOLS],
            }
        else:
            self._resp = FakeHttpResponse(404, b"{}")
            return
        self._resp = FakeHttpResponse(200, json.dumps(body).encode())

    def getresponse(self):
        return self._resp

    def close(self):
        pass


def build_cfg(tmp_path):
    cfg = default_config(tmp_path, symbols=SYMBOLS, reserve_mb=1)
    conns = []
    for c in cfg.connections:
        streams = tuple(
            replace(
                s,
                max_gap_s=None if s.max_gap_s is None else 3.0,
                first_frame_s=None if s.first_frame_s is None else 3.0,
            )
            for s in c.streams
        )
        conns.append(replace(c, streams=streams))
    return replace(
        cfg,
        connections=tuple(conns),
        snapshot_cooldown_s=1.0,
        time_probe_interval_s=1.0,
        exchange_info_interval_s=3600.0,
    )


def run_recorder(tmp_path, market, requests, until, *, timeout=60.0, overrides=None):
    cfg = build_cfg(tmp_path)
    by_url = {c.url: "/" + c.name for c in cfg.connections}

    async def main():
        server, port = await fake_exchange(market)

        def connect(url, **kw):
            kw = {k: v for k, v in kw.items() if k != "proxy" or v is None}
            return ws_connect("ws://127.0.0.1:%d%s" % (port, by_url[url]), **kw)

        rec = Recorder(
            cfg,
            clock=SYSTEM_CLOCK,
            connect=connect,
            rest_conn_factory=lambda host: MultiRestConn(market, requests),
            log=lambda m: None,
            exchange_info_delay_s=0.2,
            enable_monitor=False,
            session_overrides={
                "idle_poll_s": 0.05,
                "measure_dns": False,
                "healthy_after_s": 0.0,
                "forced_reconnect_cooldown_s": 1.0,
                "backoff_start_s": 0.1,
                "backoff_cap_s": 1.0,
                **(overrides or {}),
            },
        )
        stop = asyncio.Event()
        task = asyncio.create_task(rec.run(stop))
        end = time.monotonic() + timeout
        try:
            while time.monotonic() < end and not task.done():
                await asyncio.sleep(0.1)
                if getattr(rec, "writer", None) is not None and until(rec):
                    break
        finally:
            stop.set()
            code = await asyncio.wait_for(task, 60)
            server.close()
            await asyncio.wait_for(server.wait_closed(), 10)
        return rec, code

    return asyncio.run(main())


def rotation_gaps_s(events):
    """WS_CLOSE(reason rotation) -> next WS_OPEN of the same connection, the way hostprobe measures it."""
    closed: dict[tuple, int] = {}
    gaps = []
    for e in events:
        if e["k"] == "WS_CLOSE":
            closed[(e.get("run"), e["conn"])] = (e["t"], str(e.get("reason", "")))
        elif e["k"] == "WS_OPEN" and (e.get("run"), e["conn"]) in closed:
            t0, why = closed.pop((e.get("run"), e["conn"]))
            if why.startswith("rotation"):
                gaps.append((e["conn"], (e["t"] - t0) / 1_000_000))
    return gaps


def test_two_symbols_record_bridge_rotate_all_six_connections_and_verify(tmp_path):
    market, requests = MultiMarket(), []

    def done(rec):
        events = Ledger.read_all(tmp_path)
        rotated = {
            e["conn"]
            for e in events
            if e["k"] == "WS_CLOSE" and str(e.get("reason", "")).startswith("rotation")
        }
        bridged = Counter(e["stream"] for e in events if e["k"] == "BRIDGE_OK")
        refdata = sum(1 for e in events if e["k"] == "REFDATA")
        return rotated == CONNS and len(bridged) == 4 and min(bridged.values()) >= 2 and refdata >= 3

    rec, code = run_recorder(tmp_path, market, requests, done, overrides={"rotation_age_s": ROTATION_AGE_S})
    assert code == 0

    events = Ledger.read_all(tmp_path)
    kinds = [e["k"] for e in events]
    assert kinds[0] == "PROC_START" and kinds[-1] == "PROC_STOP"

    # -- the layout: six connections, and every one of them was opened again after its rotation ----
    opens = Counter(e["conn"] for e in events if e["k"] == "WS_OPEN")
    assert set(opens) == CONNS and min(opens.values()) >= 2, opens
    for name in DEPTH_CONNS:
        gens = [e["gen"] for e in events if e["k"] == "WS_OPEN" and e["conn"] == name]
        assert gens[:2] == [1, 2], (name, gens)

    # -- one bridge per (venue, symbol) and generation, each stream on its own ---------------------
    bridged = Counter(e["stream"] for e in events if e["k"] == "BRIDGE_OK")
    assert set(bridged) == {
        "spot:btcusdt@depth@100ms",
        "usdm:btcusdt@depth@100ms",
        "spot:ethusdt@depth@100ms",
        "usdm:ethusdt@depth@100ms",
    }
    assert min(bridged.values()) >= 2, bridged

    # -- nothing in the ledger that would have been a finding in production --------------------------
    bad = {
        "GAP_DETECTED", "STREAM_STALL", "SUBSCRIBED_NO_DATA", "BAD_FRAME", "RATE_LIMIT",
        "TASK_CRASH", "OVERRUN", "WS_CONNECT_FAIL", "BAN",
    }  # fmt: skip
    assert not (bad & set(kinds)), sorted(bad & set(kinds))

    # -- snapshots were requested per (venue, symbol), only allowlisted public GETs were used -----
    asked = {
        (urlsplit(p).path, (parse_qs(urlsplit(p).query).get("symbol") or [""])[0].upper())
        for p in requests
    }
    assert {
        ("/api/v3/depth", "BTCUSDT"),
        ("/api/v3/depth", "ETHUSDT"),
        ("/fapi/v1/depth", "BTCUSDT"),
        ("/fapi/v1/depth", "ETHUSDT"),
    } <= asked
    assert {p for p, _ in asked} <= {
        "/api/v3/depth", "/fapi/v1/depth", "/api/v3/time", "/fapi/v1/time",
        "/api/v3/exchangeInfo", "/fapi/v1/exchangeInfo",
    }  # fmt: skip

    # -- a planned rotation costs only the reconnect: every connection, well under the 1 s the doc promises --
    gaps = rotation_gaps_s(events)
    assert {c for c, _ in gaps} == CONNS, gaps
    assert max(g for _, g in gaps) < 2.0, gaps

    # -- the offline verifier proves it from the raw data, one chain per stream -------------------
    report = verify_lake(tmp_path)
    assert report.exit_code == 0, report.render_text()
    expected = {
        "%s:%s@depth@100ms" % (venue, sym.lower()) for venue in ("spot", "usdm") for sym in SYMBOLS
    } | {
        "spot:%s@trade" % sym.lower() for sym in SYMBOLS
    } | {
        "usdm:%s@aggTrade" % sym.lower() for sym in SYMBOLS
    } | {
        "usdm:%s@markPrice@1s" % sym.lower() for sym in SYMBOLS
    } | {
        "%s:snapshot:%s" % (venue, sym) for venue in ("spot", "usdm") for sym in SYMBOLS
    } | {
        "spot:exchangeInfo:BTCUSDT", "spot:exchangeInfo:ETHUSDT", "usdm:exchangeInfo",
    }  # fmt: skip
    assert set(report.streams) >= expected, sorted(expected - set(report.streams))
    for sym in SYMBOLS:
        for stream in (
            "spot:%s@depth@100ms" % sym.lower(),
            "usdm:%s@depth@100ms" % sym.lower(),
            "spot:%s@trade" % sym.lower(),
            "usdm:%s@aggTrade" % sym.lower(),
        ):
            assert report.streams[stream]["records"] > 20, stream

    # both depth connections of a venue write into the same segment class: the files really are shared
    live = list((tmp_path / "raw").rglob("*.jsonl.zst"))
    assert {p.parts[-3] for p in live} == {"depth", "trade", "market", "snapshot", "ref"}

    # -- the operations report reads the same lake ----------------------------------------------------
    rep = build_report(tmp_path, sample_every=1)
    assert rep["bridge_seconds_from_ws_open"]["n"] >= 8 and not rep["slow_bridges_over_10s"]
    assert set(rep["connections"]["opens_per_connection"]) == CONNS
    assert min(rep["connections"]["opens_per_connection"].values()) >= 2
    for stream in ("spot:ethusdt@trade", "usdm:ethusdt@aggTrade", "spot:btcusdt@trade"):
        assert rep["streams"][stream]["recv_minus_event_ms"]["n"] > 0, stream
    assert rep["rate_limits"]["http_418_bans"] == 0 and rep["rate_limits"]["http_429_403_451_events"] == 0

    # -- status.json carries the operator's view of all six connections ---------------------------
    status = json.loads((tmp_path / "status.json").read_bytes())
    assert set(status["connections"]) == CONNS and status["disk"]["stopped"] is False
