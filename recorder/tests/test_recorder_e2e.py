"""The whole recorder against a local fake exchange (real sockets, real writer thread, real files)."""

from __future__ import annotations

import asyncio
import json
import time
from dataclasses import replace
from urllib.parse import urlsplit

import websockets
from websockets.asyncio.client import connect as ws_connect
from websockets.asyncio.server import serve
from websockets.protocol import State

from hy_recorder.clock import SYSTEM_CLOCK
from hy_recorder.config import default_config
from hy_recorder.ledger import Ledger
from hy_recorder.recorder import Recorder
from hy_recorder.verify import verify_lake


class FakeMarket:
    """Shared truth of the fake exchange: monotone ids, so a snapshot can be made to bridge."""

    def __init__(self):
        self.spot_u = 1000
        self.usdm_u = 5000
        self.trade_id = 0
        self.agg_id = 0
        self.mark_e = 1_790_000_000_000
        self.skip_spot_depth_once = False
        self.skip_done = False
        self.silent_market = False
        self.connections: list[str] = []

    def spot_depth(self):
        lo = self.spot_u + 1
        if self.skip_spot_depth_once and not self.skip_done:
            self.skip_done = True
            self.spot_u += 7  # an event the recorder never sees
            lo = self.spot_u + 1
        self.spot_u = lo + 1
        return {
            "e": "depthUpdate",
            "E": int(time.time() * 1000),
            "s": "BTCUSDT",
            "U": lo,
            "u": self.spot_u,
            "b": [["1.0", "1"]],
            "a": [["2.0", "1"]],
        }

    def usdm_depth(self):
        lo = self.usdm_u + 1
        pu = self.usdm_u
        self.usdm_u = lo + 2  # width 3: [lo, lo+2]; a snapshot at last_u-1 always falls inside the last event
        return {
            "e": "depthUpdate",
            "E": int(time.time() * 1000),
            "T": int(time.time() * 1000),
            "s": "BTCUSDT",
            "U": lo,
            "u": self.usdm_u,
            "pu": pu,
            "b": [["1.0", "1"]],
            "a": [["2.0", "1"]],
        }

    def trade(self):
        self.trade_id += 1
        now = int(time.time() * 1000)
        return {
            "e": "trade",
            "E": now,
            "s": "BTCUSDT",
            "t": self.trade_id,
            "p": "100.0",
            "q": "0.1",
            "T": now,
            "m": True,
            "M": True,
        }

    def agg(self):
        self.agg_id += 1
        now = int(time.time() * 1000)
        return {
            "e": "aggTrade",
            "E": now,
            "s": "BTCUSDT",
            "a": self.agg_id,
            "p": "100.0",
            "q": "0.1",
            "f": self.agg_id * 10,
            "l": self.agg_id * 10,
            "T": now,
            "m": False,
        }

    def mark(self):
        self.mark_e += 1000
        return {
            "e": "markPriceUpdate",
            "E": self.mark_e,
            "s": "BTCUSDT",
            "p": "100.0",
            "i": "100.0",
            "P": "100.0",
            "r": "0.0001",
            "T": self.mark_e + 1000,
        }


def frame(stream, data):
    return json.dumps({"stream": stream, "data": data}, separators=(",", ":"))


async def fake_exchange(market: FakeMarket):
    """One local server; the request path says which fake stream family to serve."""

    async def handler(ws):
        path = urlsplit(ws.request.path).path
        market.connections.append(path)
        try:
            if path == "/spot_depth":
                while ws.state is State.OPEN:
                    await ws.send(frame("btcusdt@depth@100ms", market.spot_depth()))
                    await asyncio.sleep(0.004)
            elif path == "/usdm_depth":
                while ws.state is State.OPEN:
                    await ws.send(frame("btcusdt@depth@100ms", market.usdm_depth()))
                    await asyncio.sleep(0.004)
            elif path == "/spot_trade":
                while ws.state is State.OPEN:
                    await ws.send(frame("btcusdt@trade", market.trade()))
                    await asyncio.sleep(0.005)
            elif path == "/usdm_market":
                n = 0
                while (
                    ws.state is State.OPEN
                ):  # a silent market never sends, so it must watch the state itself
                    if not market.silent_market:
                        await ws.send(frame("btcusdt@aggTrade", market.agg()))
                        n += 1
                        if n % 10 == 0:
                            await ws.send(frame("btcusdt@markPrice@1s", market.mark()))
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


class FakeRestConn:
    """Serves the allowlisted GETs from the fake market; can be told to fail or rate limit."""

    def __init__(self, market: FakeMarket, plan):
        self.market, self.plan, self._resp = market, plan, None

    def request(self, method, path, headers=None):
        assert method == "GET" and set(headers) <= {"User-Agent", "Accept"}
        self.plan.requests.append(path)
        if self.plan.fail_next_depth > 0 and "/depth" in path:
            self.plan.fail_next_depth -= 1
            self._resp = FakeHttpResponse(500, b'{"code":-1,"msg":"boom"}')
            return
        if self.plan.rate_limit_next > 0:
            self.plan.rate_limit_next -= 1
            self._resp = FakeHttpResponse(429, b'{"code":-1003,"msg":"too many"}', {"retry-after": "3"})
            return
        p = urlsplit(path).path
        m = self.market
        if p == "/api/v3/depth":
            body = {"lastUpdateId": m.spot_u, "bids": [["1.0", "1"]], "asks": [["2.0", "1"]]}
        elif p == "/fapi/v1/depth":
            body = {
                "lastUpdateId": m.usdm_u - 1,
                "E": 1,
                "T": 1,
                "bids": [["1.0", "1"]],
                "asks": [["2.0", "1"]],
            }
        elif p in ("/api/v3/time", "/fapi/v1/time"):
            body = {"serverTime": int(time.time() * 1000) + 12}
        elif p == "/api/v3/exchangeInfo":
            body = {"timezone": "UTC", "symbols": [{"symbol": "BTCUSDT", "filters": []}]}
        elif p == "/fapi/v1/exchangeInfo":
            body = {"timezone": "UTC", "symbols": [{"symbol": "BTCUSDT", "contractType": "PERPETUAL"}]}
        else:
            self._resp = FakeHttpResponse(404, b"{}")
            return
        self._resp = FakeHttpResponse(200, json.dumps(body).encode())

    def getresponse(self):
        return self._resp

    def close(self):
        pass


class RestPlan:
    def __init__(self):
        self.requests: list[str] = []
        self.fail_next_depth = 0
        self.rate_limit_next = 0


def build_cfg(tmp_path, **kw):
    cfg = default_config(tmp_path, **kw)
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


LOCAL_PATH = {
    "spot_depth_btcusdt": "/spot_depth",
    "usdm_depth_btcusdt": "/usdm_depth",
    "spot_trade": "/spot_trade",
    "usdm_market": "/usdm_market",
}


def run_recorder(tmp_path, market, plan, until, *, timeout=40.0, cfg_kw=None, overrides=None):
    """Run a Recorder against the fake exchange until ``until(recorder)`` holds (or timeout), then stop it."""
    cfg = build_cfg(tmp_path, **(cfg_kw or {}))
    by_url = {c.url: LOCAL_PATH[c.name] for c in cfg.connections}

    async def main():
        server, port = await fake_exchange(market)

        def connect(url, **kw):
            kw = {k: v for k, v in kw.items() if k != "proxy" or v is None}
            return ws_connect("ws://127.0.0.1:%d%s" % (port, by_url[url]), **kw)

        rec = Recorder(
            cfg,
            clock=SYSTEM_CLOCK,
            connect=connect,
            rest_conn_factory=lambda host: FakeRestConn(market, plan),
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


def ledger_kinds(root):
    return [e["k"] for e in Ledger.read_all(root)]


def bridged(root, venue):
    return [e for e in Ledger.read_all(root) if e["k"] == "BRIDGE_OK" and e["stream"].startswith(venue + ":")]


def test_full_pipeline_records_bridges_verifies_and_shuts_down_cleanly(tmp_path):
    market, plan = FakeMarket(), RestPlan()

    def done(rec):
        kinds = ledger_kinds(tmp_path)
        return (
            kinds.count("BRIDGE_OK") >= 2
            and "CLOCK_PROBE" in kinds
            and "REFDATA" in kinds
            and rec.writer.status_snapshot().get("queues", {}).get("usdm/market", {}).get("written", 0) > 60
        )

    rec, code = run_recorder(tmp_path, market, plan, done)
    assert code == 0
    kinds = ledger_kinds(tmp_path)
    assert kinds[0] == "PROC_START" and kinds[-1] == "PROC_STOP" and "HOST_PROFILE" in kinds
    assert kinds.count("WS_OPEN") == 4 and kinds.count("BRIDGE_OK") >= 2
    assert {"CLOCK_PROBE", "REFDATA", "SNAPSHOT"} <= set(kinds)
    assert "GAP_DETECTED" not in kinds and "SUBSCRIBED_NO_DATA" not in kinds and "STREAM_STALL" not in kinds

    report = verify_lake(tmp_path)
    assert report.exit_code == 0, report.render_text()
    assert set(report.streams) >= {
        "spot:btcusdt@depth@100ms",
        "usdm:btcusdt@depth@100ms",
        "spot:btcusdt@trade",
        "usdm:btcusdt@aggTrade",
        "usdm:btcusdt@markPrice@1s",
        "spot:snapshot:BTCUSDT",
        "usdm:snapshot:BTCUSDT",
        "spot:exchangeInfo:BTCUSDT",
        "usdm:exchangeInfo",
    }
    assert report.runs[1]["clean"] is True
    # the bridge is proven from the raw data by the offline verifier, independent of the online events
    depth = report.streams["spot:btcusdt@depth@100ms"]
    assert depth["records"] > 20
    # only REST-derived reference data and snapshots go to their own classes; websocket data never does
    live = list((tmp_path / "raw").rglob("*.jsonl.zst"))
    classes = {p.parts[-3] for p in live}
    assert classes == {"depth", "trade", "market", "snapshot", "ref"}
    # status.json carries the operator's view
    status = json.loads((tmp_path / "status.json").read_bytes())
    assert status["run"] == 1 and set(status["connections"]) == set(LOCAL_PATH)
    assert status["disk"]["stopped"] is False and status["rest"]["counters"] == {
        "429": 0,
        "418": 0,
        "403": 0,
        "451": 0,
    }


def test_every_rest_request_was_an_allowlisted_public_get(tmp_path):
    market, plan = FakeMarket(), RestPlan()
    rec, _ = run_recorder(
        tmp_path,
        market,
        plan,
        lambda r: "REFDATA" in ledger_kinds(tmp_path) and ledger_kinds(tmp_path).count("BRIDGE_OK") >= 2,
    )
    assert plan.requests, "the recorder never touched REST"
    for path in plan.requests:
        p = urlsplit(path).path
        assert p in {
            "/api/v3/depth",
            "/fapi/v1/depth",
            "/api/v3/time",
            "/fapi/v1/time",
            "/api/v3/exchangeInfo",
            "/fapi/v1/exchangeInfo",
        }, path


def test_a_missed_depth_event_is_detected_reconnected_and_rebridged_with_the_gap_explained(tmp_path):
    market, plan = FakeMarket(), RestPlan()

    def done(rec):
        if market.skip_done is False and ledger_kinds(tmp_path).count("BRIDGE_OK") >= 2:
            market.skip_spot_depth_once = True  # both bridged: now lose one event on the spot depth stream
        if not market.skip_done:
            return False
        kinds = ledger_kinds(tmp_path)
        return (
            "GAP_DETECTED" in kinds
            and len(
                [
                    e
                    for e in Ledger.read_all(tmp_path)
                    if e["k"] == "BRIDGE_OK" and e["stream"].startswith("spot:")
                ]
            )
            >= 2
        )

    rec, code = run_recorder(tmp_path, market, plan, done, timeout=60)
    assert code == 0
    events = Ledger.read_all(tmp_path)
    gaps = [e for e in events if e["k"] == "GAP_DETECTED"]
    assert gaps and gaps[0]["stream"] == "spot:btcusdt@depth@100ms" and gaps[0]["gen"] == 1
    opens = [e for e in events if e["k"] == "WS_OPEN" and e["conn"] == "spot_depth_btcusdt"]
    assert [e["gen"] for e in opens][:2] == [1, 2], (
        "the gap must trigger a fresh generation on that connection only"
    )
    assert [e["gen"] for e in events if e["k"] == "WS_OPEN" and e["conn"] == "usdm_depth_btcusdt"] == [1]
    closes = [e for e in events if e["k"] == "WS_CLOSE" and e["conn"] == "spot_depth_btcusdt"]
    assert closes[0]["reason"].startswith("gap:")
    report = verify_lake(tmp_path)
    # the raw data proves the hole; the ledger explains it; nothing may be unexplained
    assert not [i for i in report.issues if i.severity == "FAIL"], report.render_text()
    assert any(i.code.startswith("DEPTH_GAP") or "GAP" in i.code for i in report.issues), (
        "the gap must be visible"
    )


def test_a_failing_snapshot_is_retried_with_backoff_and_the_generation_still_bridges(tmp_path):
    market, plan = FakeMarket(), RestPlan()
    plan.fail_next_depth = 3

    rec, code = run_recorder(
        tmp_path, market, plan, lambda r: ledger_kinds(tmp_path).count("BRIDGE_OK") >= 2, timeout=90
    )
    assert code == 0
    events = Ledger.read_all(tmp_path)
    fails = [e for e in events if e["k"] == "SNAPSHOT_FAIL"]
    assert len(fails) == 3 and all(e["status"] == 500 for e in fails)
    assert [e["backoff_s"] for e in fails if e["stream"] == fails[0]["stream"]][:1] == [5.0]
    assert len([e for e in events if e["k"] == "BRIDGE_OK"]) >= 2, (
        "no attempt cap: bridging must eventually succeed"
    )
    report = verify_lake(tmp_path)
    assert report.exit_code == 0, report.render_text()


def test_a_429_puts_rest_to_sleep_but_recording_continues(tmp_path):
    market, plan = FakeMarket(), RestPlan()
    plan.rate_limit_next = 1  # the very first REST call is answered with 429
    sample: dict = {}

    def done(rec):
        if "RATE_LIMIT" not in ledger_kinds(tmp_path):
            return False
        if not sample:
            sample["t"], sample["n"] = time.monotonic(), len(plan.requests)
        written = rec.writer.status_snapshot().get("queues", {}).get("spot/trade", {}).get("written", 0)
        return time.monotonic() - sample["t"] >= 2.5 and written > 100

    rec, code = run_recorder(tmp_path, market, plan, done, timeout=60)
    assert code == 0
    events = Ledger.read_all(tmp_path)
    rl = [e for e in events if e["k"] == "RATE_LIMIT"]
    assert rl and rl[0]["status"] == 429 and rl[0]["sleep_s"] >= 300
    assert len(plan.requests) <= sample["n"] + 1, (
        "REST must be silent during the deep sleep: %d requests when it began, %d at the end"
        % (sample["n"], len(plan.requests))
    )
    report = verify_lake(tmp_path)
    assert report.streams["spot:btcusdt@trade"]["records"] > 100
    assert json.loads((tmp_path / "status.json").read_bytes())["rest"]["counters"]["429"] == 1
    assert not [i for i in report.issues if i.severity == "FAIL"], report.render_text()


def test_a_silent_stream_is_reported_and_only_that_connection_is_affected(tmp_path):
    market, plan = FakeMarket(), RestPlan()
    market.silent_market = True

    def done(rec):
        kinds = ledger_kinds(tmp_path)
        return "SUBSCRIBED_NO_DATA" in kinds and kinds.count("BRIDGE_OK") >= 2

    rec, code = run_recorder(tmp_path, market, plan, done, timeout=60)
    assert code == 0
    events = Ledger.read_all(tmp_path)
    nd = [e for e in events if e["k"] == "SUBSCRIBED_NO_DATA"]
    assert {e["stream"] for e in nd} == {"usdm:btcusdt@aggTrade", "usdm:btcusdt@markPrice@1s"}
    assert [e["gen"] for e in events if e["k"] == "WS_OPEN" and e["conn"] == "spot_trade"] == [1]


def test_restart_recovers_a_crashed_run_and_numbers_continue(tmp_path):
    market, plan = FakeMarket(), RestPlan()
    run_recorder(tmp_path, market, plan, lambda r: ledger_kinds(tmp_path).count("BRIDGE_OK") >= 2)
    # simulate a crash of the first run: a .part file with a torn tail and no PROC_STOP / clean flag
    import zstandard

    part_dir = tmp_path / "raw" / "spot" / "trade" / "20260929"
    part_dir.mkdir(parents=True, exist_ok=True)
    body = (
        b'{"v":1,"k":"m","r":1,"q":99999,"g":1,"t":1790000000000000,"m":1,"s":"spot:btcusdt@trade","p":"x"}\n'
    )
    comp = zstandard.ZstdCompressor(level=3).compressobj()
    blob = comp.compress(body + b'{"v":1,"k":"m","r":1,"q":100000,"g":1,"t":17900000') + comp.flush(
        zstandard.COMPRESSOBJ_FLUSH_BLOCK
    )
    (part_dir / "10-777777.jsonl.zst.part").write_bytes(blob)
    market2, plan2 = FakeMarket(), RestPlan()
    rec, code = run_recorder(
        tmp_path,
        market2,
        plan2,
        lambda r: (
            "SEGMENT_RECOVERED" in ledger_kinds(tmp_path) and ledger_kinds(tmp_path).count("BRIDGE_OK") >= 4
        ),
        timeout=60,
    )
    assert code == 0 and rec.run_no == 2
    events = Ledger.read_all(tmp_path)
    assert [e["run"] for e in events if e["k"] == "PROC_START"] == [1, 2]
    rec_ev = [e for e in events if e["k"] == "SEGMENT_RECOVERED"]
    assert rec_ev and rec_ev[0]["truncated"] is True
    assert not list((tmp_path / "raw").rglob("*.part"))
    assert (tmp_path / "recovered" / "10-777777.jsonl.zst.part").exists()
