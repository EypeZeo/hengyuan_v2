from __future__ import annotations

import asyncio
import base64
import hashlib
import json
import random
import time
from collections import deque

import pytest
from websockets.asyncio.client import connect as ws_connect
from websockets.exceptions import ConnectionClosedError

import hy_recorder.session as session_mod
from hy_recorder.clock import SYSTEM_CLOCK
from hy_recorder.config import ConnectionSpec, StreamSpec
from hy_recorder.depthtrack import DepthTracker
from hy_recorder.envelope import decode_record
from hy_recorder.ratelimit import RestGovernor
from hy_recorder.seq import SeqCounter
from hy_recorder.session import ConnectionManager, SessionEnv
from hy_recorder.state import StateFile

TRADE_URL = "wss://stream.binance.com:9443/stream?streams=btcusdt@trade"
DEPTH_URL = "wss://stream.binance.com:9443/stream?streams=btcusdt@depth@100ms"


def trade_spec(max_gap=0.3, first=0.3):
    return ConnectionSpec(
        "spot_trade",
        "spot",
        "trade",
        TRADE_URL,
        (StreamSpec("btcusdt@trade", "trade", "BTCUSDT", max_gap, first),),
    )


def depth_spec():
    return ConnectionSpec(
        "spot_depth_btcusdt",
        "spot",
        "depth",
        DEPTH_URL,
        (StreamSpec("btcusdt@depth@100ms", "depth", "BTCUSDT", 5.0, 20.0),),
    )


def trade_frame(t):
    return json.dumps({"stream": "btcusdt@trade", "data": {"e": "trade", "t": t}})


def depth_frame(U, u):
    return json.dumps({"stream": "btcusdt@depth@100ms", "data": {"e": "depthUpdate", "U": U, "u": u}})


class FakeWs:
    """A scripted websocket: ``feed`` queues messages, ``server_close`` ends the connection."""

    def __init__(self):
        self.q: asyncio.Queue = asyncio.Queue()

    def feed(self, *msgs):
        for m in msgs:
            self.q.put_nowait(m)

    def server_close(self):
        self.q.put_nowait(ConnectionClosedError(None, None))

    async def recv(self):
        item = await self.q.get()
        if isinstance(item, Exception):
            raise item
        return item

    async def __aenter__(self):
        return self

    async def __aexit__(self, *exc):
        return False


class FakeWriter:
    def __init__(self):
        self.records = []
        self.accept = True

    def submit_record(self, venue, cls, line, *, seq, wall_us, stream):
        self.records.append((venue, cls, line, seq, wall_us, stream))
        return self.accept


class Harness:
    def __init__(self, tmp_path, spec, connect, **env_kw):
        self.events: list[dict] = []
        self.state = StateFile(tmp_path)
        self.state.load()
        self.state.begin_run()
        self.gov = RestGovernor(SYSTEM_CLOCK, self.state, self.emit)
        self.stop = asyncio.Event()
        self.writer = FakeWriter()
        self.seq = SeqCounter()
        kw = dict(
            idle_poll_s=0.02,
            forced_reconnect_cooldown_s=0.3,
            measure_dns=False,
            rng=random.Random(7),
            healthy_after_s=0.0,
        )
        kw.update(env_kw)
        self.env = SessionEnv(
            clock=SYSTEM_CLOCK,
            seq=self.seq,
            run_no=1,
            writer=self.writer,
            emit=self.emit,
            governor=self.gov,
            stop=self.stop,
            connect=connect,
            **kw,
        )
        self.mgr = ConnectionManager(spec, self.env)

    def emit(self, kind, **fields):
        self.events.append({"k": kind, **fields})

    def kinds(self):
        return [e["k"] for e in self.events]

    def of(self, kind):
        return [e for e in self.events if e["k"] == kind]


async def wait_until(pred, timeout=3.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if pred():
            return True
        await asyncio.sleep(0.01)
    return False


def scripted_connect(sockets):
    """Return a ``connect`` that hands out the given sockets in order (then blocks forever on failure)."""
    it = iter(sockets)
    calls = []

    def connect(url, **kw):
        calls.append((url, kw))
        item = next(it)
        if isinstance(item, Exception):
            raise item
        return item

    connect.calls = calls
    return connect


def drive(tmp_path, spec, sockets, scenario, **env_kw):
    """Run the manager against scripted sockets; ``scenario(h)`` is awaited, then the manager is stopped."""
    connect = scripted_connect(sockets)

    async def main():
        h = Harness(tmp_path, spec, connect, **env_kw)
        task = asyncio.create_task(h.mgr.run())
        try:
            await scenario(h)
        finally:
            h.stop.set()
            await asyncio.wait_for(task, 10)
        return h

    h = asyncio.run(main())
    return h, connect


def test_records_are_stamped_encoded_and_submitted_in_order(tmp_path):
    ws = FakeWs()

    async def scenario(h):
        ws.feed(*[trade_frame(i) for i in range(1, 6)])
        assert await wait_until(lambda: len(h.writer.records) == 5)

    h, connect = drive(tmp_path, trade_spec(), [ws], scenario)
    recs = [decode_record(r[2]) for r in h.writer.records]
    assert [r.seq for r in recs] == sorted(r.seq for r in recs) and len({r.seq for r in recs}) == 5
    assert all(r.stream == "spot:btcusdt@trade" and r.gen == 1 and r.run == 1 for r in recs)
    assert [json.loads(r.payload)["data"]["t"] for r in recs] == [1, 2, 3, 4, 5]
    assert all(r.wall_us > 0 and r.mono_us > 0 for r in recs)
    ws_open = h.of("WS_OPEN")
    assert len(ws_open) == 1 and ws_open[0]["gen"] == 1 and ws_open[0]["streams"] == ["spot:btcusdt@trade"]
    # WS_OPEN was drawn before the first record: it precedes every record of its generation in seq order
    assert h.kinds().index("WS_OPEN") == 0
    kw = connect.calls[0][1]
    assert kw["proxy"] is None and kw["ping_interval"] is None and kw["compression"] is None
    assert kw["close_timeout"] <= 0.5, (
        "a long close timeout holds up every reconnect (the 5.3 s rotation hole)"
    )


def test_server_close_starts_a_new_generation_and_records_the_reason(tmp_path):
    ws1, ws2 = FakeWs(), FakeWs()

    async def scenario(h):
        ws1.feed(trade_frame(1))
        assert await wait_until(lambda: len(h.writer.records) == 1)
        ws1.server_close()
        ws2.feed(trade_frame(1000))
        assert await wait_until(lambda: len(h.writer.records) == 2)

    h, _ = drive(tmp_path, trade_spec(), [ws1, ws2], scenario)
    assert [e["gen"] for e in h.of("WS_OPEN")] == [1, 2]
    close = h.of("WS_CLOSE")[0]
    assert close["gen"] == 1 and close["reason"].startswith("closed")
    assert [decode_record(r[2]).gen for r in h.writer.records] == [1, 2]


def test_a_stream_that_had_data_and_went_quiet_is_a_stall_and_reconnects(tmp_path):
    ws1, ws2 = FakeWs(), FakeWs()

    async def scenario(h):
        ws1.feed(trade_frame(1))
        assert await wait_until(lambda: h.of("STREAM_STALL"), 3)  # max_gap 0.3 s
        assert await wait_until(lambda: len(h.of("WS_OPEN")) == 2, 3)

    h, _ = drive(tmp_path, trade_spec(max_gap=0.3), [ws1, ws2], scenario)
    stall = h.of("STREAM_STALL")[0]
    assert stall["stream"] == "spot:btcusdt@trade" and stall["gen"] == 1 and stall["silent_s"] >= 0.3
    assert h.of("WS_CLOSE")[0]["reason"] == "stall:spot:btcusdt@trade"


def test_subscribed_but_never_delivering_is_flagged_and_only_retried_at_the_slow_interval(
    tmp_path, monkeypatch
):
    monkeypatch.setattr(session_mod, "NO_DATA_RETRY_S", 0.8)
    ws1, ws2 = FakeWs(), FakeWs()

    async def scenario(h):
        assert await wait_until(lambda: h.of("SUBSCRIBED_NO_DATA"), 3)  # first_frame 0.3 s
        assert len(h.of("WS_OPEN")) == 1, "the connection must not be torn down at the first-frame deadline"
        assert await wait_until(lambda: len(h.of("WS_OPEN")) == 2, 4)  # retried after NO_DATA_RETRY_S

    h, _ = drive(tmp_path, trade_spec(first=0.3), [ws1, ws2], scenario)
    nd = h.of("SUBSCRIBED_NO_DATA")[0]
    assert nd["stream"] == "spot:btcusdt@trade" and nd["gen"] == 1 and nd["waited_s"] >= 0.3
    assert h.of("WS_CLOSE")[0]["reason"] == "no_data_retry"
    snap = h.mgr.snapshot()
    assert snap["streams"]["spot:btcusdt@trade"]["healthy"] is False or snap["gen"] == 2


def test_no_data_flag_is_emitted_once_per_generation(tmp_path, monkeypatch):
    monkeypatch.setattr(session_mod, "NO_DATA_RETRY_S", 5.0)
    ws1 = FakeWs()

    async def scenario(h):
        assert await wait_until(lambda: h.of("SUBSCRIBED_NO_DATA"), 3)
        await asyncio.sleep(0.5)

    h, _ = drive(tmp_path, trade_spec(first=0.2), [ws1], scenario)
    assert len(h.of("SUBSCRIBED_NO_DATA")) == 1


def test_a_stream_without_a_liveness_expectation_is_never_stalled(tmp_path):
    spec = ConnectionSpec(
        "m",
        "usdm",
        "market",
        "wss://fstream.binance.com/market/stream?streams=!forceOrder@arr",
        (StreamSpec("!forceOrder@arr", "forceOrder", None, None, None),),
    )
    ws = FakeWs()

    async def scenario(h):
        await asyncio.sleep(0.6)

    h, _ = drive(tmp_path, spec, [ws], scenario)
    assert not h.of("STREAM_STALL") and not h.of("SUBSCRIBED_NO_DATA") and len(h.of("WS_OPEN")) == 1


def test_rotation_reconnects_after_the_configured_age(tmp_path):
    ws1, ws2 = FakeWs(), FakeWs()

    async def scenario(h):
        for i in range(60):  # keep the stream healthy while the connection ages
            ws1.feed(trade_frame(i))
            await asyncio.sleep(0.01)
        assert await wait_until(lambda: len(h.of("WS_OPEN")) == 2, 3)

    h, _ = drive(tmp_path, trade_spec(max_gap=5, first=5), [ws1, ws2], scenario, rotation_age_s=0.3)
    assert h.of("WS_CLOSE")[0]["reason"] == "rotation"


class TimedHarness(Harness):
    """A Harness whose ledger events carry the monotonic time they were emitted at."""

    def emit(self, kind, **fields):
        self.events.append({"k": kind, "mono": time.monotonic(), **fields})


def test_a_planned_rotation_reconnects_without_sleeping_first(tmp_path):
    ws1, ws2 = FakeWs(), FakeWs()
    connect = scripted_connect([ws1, ws2])

    async def main():
        h = TimedHarness(tmp_path, trade_spec(max_gap=5, first=5), connect, rotation_age_s=0.3)
        task = asyncio.create_task(h.mgr.run())
        try:
            for i in range(60):  # keep the stream healthy while the connection ages
                ws1.feed(trade_frame(i))
                await asyncio.sleep(0.01)
            assert await wait_until(lambda: len(h.of("WS_OPEN")) == 2, 3)
        finally:
            h.stop.set()
            await asyncio.wait_for(task, 10)
        return h

    h = asyncio.run(main())
    gap = h.of("WS_OPEN")[1]["mono"] - h.of("WS_CLOSE")[0]["mono"]
    # the former jittered sleep (0.2 s x 0.5..1.5) alone made this at least 0.1 s
    assert gap < 0.08, "a planned rotation slept %.3f s between WS_CLOSE and the next WS_OPEN" % gap


_WS_GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


async def _serve_trades_and_ignore_close(writers):
    """A WebSocket server that, like Binance, never closes the TCP stream after the client's close
    frame: it keeps streaming trade frames and never reads again, so only the client's own close
    timeout ends the wait."""

    async def handle(reader, writer):
        writers.append(writer)
        request = await reader.readuntil(b"\r\n\r\n")
        key = next(
            line.split(b":", 1)[1].strip()
            for line in request.split(b"\r\n")
            if line.lower().startswith(b"sec-websocket-key:")
        )
        accept = base64.b64encode(hashlib.sha1(key + _WS_GUID).digest())
        writer.write(
            b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            b"Sec-WebSocket-Accept: " + accept + b"\r\n\r\n"
        )
        t = 0
        try:
            while True:
                payload = trade_frame(t).encode()
                writer.write(
                    bytes([0x81, len(payload)]) + payload
                )  # one unmasked text frame, payload < 126 bytes
                await writer.drain()
                t += 1
                await asyncio.sleep(0.01)
        except OSError:
            pass

    server = await asyncio.start_server(handle, "127.0.0.1", 0)
    return server, server.sockets[0].getsockname()[1]


def test_rotation_does_not_wait_for_a_server_that_ignores_our_close_frame(tmp_path):
    """The D0 host showed WS_CLOSE -> WS_OPEN = 5.15-5.53 s at every rotation: 5.0 s of it was the
    close timeout (the server never closed the TCP stream), during which the new connection could
    not start."""
    writers: list = []

    async def main():
        server, port = await _serve_trades_and_ignore_close(writers)
        spec = ConnectionSpec(
            "spot_trade",
            "spot",
            "trade",
            "ws://127.0.0.1:%d/stream?streams=btcusdt@trade" % port,
            (StreamSpec("btcusdt@trade", "trade", "BTCUSDT", 5.0, 5.0),),
        )
        h = TimedHarness(tmp_path, spec, ws_connect, rotation_age_s=0.4)
        task = asyncio.create_task(h.mgr.run())
        try:
            assert await wait_until(lambda: len(h.of("WS_OPEN")) == 2, 15)
        finally:
            h.stop.set()
            await asyncio.wait_for(task, 20)
            for w in writers:
                w.close()
            server.close()
        return h

    h = asyncio.run(main())
    assert h.of("WS_CLOSE")[0]["reason"] == "rotation"
    gap = h.of("WS_OPEN")[1]["mono"] - h.of("WS_CLOSE")[0]["mono"]
    assert gap < 1.5, "rotation took %.2f s from WS_CLOSE to the next WS_OPEN" % gap


def test_forced_reconnect_requests_are_rate_limited_then_applied_when_the_cooldown_ends(tmp_path):
    ws1, ws2, ws3 = FakeWs(), FakeWs(), FakeWs()

    async def scenario(h):
        for w in (ws1, ws2, ws3):
            w.feed(trade_frame(1))
        assert await wait_until(lambda: len(h.writer.records) >= 1)
        assert h.mgr.request_reconnect("gap:first") is True
        assert await wait_until(lambda: len(h.of("WS_OPEN")) == 2, 3)
        t_second = time.monotonic()
        assert (
            h.mgr.request_reconnect("gap:second") is False
        )  # inside the 0.3 s cooldown: deferred, not dropped
        assert await wait_until(lambda: len(h.of("WS_OPEN")) == 3, 3)
        assert time.monotonic() - t_second >= 0.2

    h, _ = drive(
        tmp_path, trade_spec(max_gap=5, first=5), [ws1, ws2, ws3], scenario, forced_reconnect_cooldown_s=0.6
    )
    assert [e["reason"] for e in h.of("WS_CLOSE")][:2] == ["gap:first", "gap:second"]


def test_a_failing_endpoint_backs_off_instead_of_storming(tmp_path):
    boom = OSError("connection refused")
    ws = FakeWs()
    stamps = []
    it = iter([boom, boom, boom, boom, ws])

    def connect(url, **kw):
        stamps.append(time.monotonic())
        item = next(it)
        if isinstance(item, Exception):
            raise item
        return item

    async def main():
        h = Harness(tmp_path, trade_spec(), connect, backoff_start_s=0.1, backoff_cap_s=0.8)
        task = asyncio.create_task(h.mgr.run())
        assert await wait_until(lambda: h.of("WS_OPEN"), 20)
        h.stop.set()
        await asyncio.wait_for(task, 10)
        return h

    h = asyncio.run(main())
    fails = h.of("WS_CONNECT_FAIL")
    assert len(fails) == 4 and all(f["error"] == "OSError" for f in fails)
    gaps = [b - a for a, b in zip(stamps, stamps[1:], strict=False)]
    nominal = [0.2, 0.4, 0.8, 0.8]  # doubling from 0.1, capped at 0.8; each delay is jittered to 0.5x..1.5x
    assert len(gaps) == 4
    for gap, want in zip(gaps, nominal, strict=True):
        assert want * 0.5 - 0.02 <= gap <= want * 1.5 + 0.4, (
            "gap %.3f outside jittered backoff of %.1f: %r" % (gap, want, gaps)
        )


def test_handshake_429_puts_the_governor_to_sleep_and_spaces_reconnects(tmp_path):
    class Resp:
        status_code = 429
        headers = {"Retry-After": "7"}

    class Rejected(Exception):
        response = Resp()

    ws = FakeWs()

    async def scenario(h):
        assert await wait_until(lambda: h.of("WS_CONNECT_FAIL"), 3)

    h, connect = drive(tmp_path, trade_spec(), [Rejected("HTTP 429"), ws], scenario)
    assert h.gov.sleep_remaining_s() >= 299
    fail = h.of("WS_CONNECT_FAIL")[0]
    assert fail["status"] == 429
    rl = h.of("RATE_LIMIT")[0]
    assert rl["status"] == 429 and rl["sleep_s"] >= 300 and rl["retry_after_s"] == 7.0
    attempts = deque([time.monotonic()])
    assert h.mgr._gate(attempts) > 25, "while REST sleeps, websocket reconnects are spaced >= 30 s apart"
    assert len(connect.calls) == 1, "and the second attempt has not happened yet"


def test_a_ban_freezes_websocket_reconnects_entirely(tmp_path):
    ws = FakeWs()

    async def main():
        connect = scripted_connect([ws])
        h = Harness(tmp_path, trade_spec(), connect)
        h.gov.on_response("spot", 418, {"retry-after": "600"})
        task = asyncio.create_task(h.mgr.run())
        await asyncio.sleep(0.5)
        state = h.mgr.state
        h.stop.set()
        await asyncio.wait_for(task, 10)
        return h, connect, state

    h, connect, state = asyncio.run(main())
    assert connect.calls == [] and state == "waiting"
    assert h.gov.ban_remaining_s() > 500


def test_attempts_are_capped_per_window(tmp_path):
    h = Harness(tmp_path, trade_spec(), scripted_connect([]))
    attempts = deque(time.monotonic() - i for i in range(session_mod.MAX_ATTEMPTS_PER_WINDOW))
    assert h.mgr._gate(attempts) > 0
    assert h.mgr._gate(deque()) == 0


def test_undecodable_and_unnamed_frames_are_recorded_and_reported_with_a_rate_limit(tmp_path):
    ws = FakeWs()

    async def scenario(h):
        ws.feed("not json at all", json.dumps({"no_stream": 1}), b"\x00\xffbinary")
        ws.feed(*["also not json"] * 40)
        assert await wait_until(lambda: len(h.writer.records) == 43)

    h, _ = drive(tmp_path, trade_spec(max_gap=5, first=5), [ws], scenario)
    recs = [decode_record(r[2]) for r in h.writer.records]
    assert all(r.stream == "spot:unknown" for r in recs)
    assert recs[0].payload == b"not json at all" and recs[2].fallback_reason == "binary"
    bad = h.of("BAD_FRAME")
    assert 1 <= len(bad) <= session_mod.BAD_FRAME_EMITS_PER_MIN, "reports are rate limited, records are not"
    assert bad[0]["bad_q"] == h.writer.records[0][3]


def test_a_configured_stream_family_that_is_not_expected_is_reported_once(tmp_path):
    ws = FakeWs()
    other = json.dumps({"stream": "ethusdt@trade", "data": {"e": "trade", "t": 1}})

    async def scenario(h):
        ws.feed(other, other, other)
        assert await wait_until(lambda: len(h.writer.records) == 3)

    h, _ = drive(tmp_path, trade_spec(max_gap=5, first=5), [ws], scenario)
    assert len(h.of("UNEXPECTED_STREAM")) == 1
    assert {decode_record(r[2]).stream for r in h.writer.records} == {"spot:ethusdt@trade"}


def test_records_the_writer_refused_are_not_fed_to_the_depth_tracker(tmp_path):
    """The online tracker follows what was actually recorded: a refused frame must show up as a gap."""
    ws = FakeWs()
    events = []
    gaps = []

    async def scenario(h):
        tracker = DepthTracker(
            "spot",
            "BTCUSDT",
            "spot:btcusdt@depth@100ms",
            lambda k, **f: events.append((k, f)),
            on_first_event=lambda g: None,
            on_bridged=lambda g: None,
            on_gap=gaps.append,
        )
        h.env.trackers["spot:btcusdt@depth@100ms"] = tracker
        ws.feed(depth_frame(1, 5))
        assert await wait_until(lambda: len(h.writer.records) == 1)
        tracker.on_snapshot(1, 3, 0)  # bridges on the first event (U<=4<=u)
        h.writer.accept = False
        ws.feed(depth_frame(6, 8))  # refused by the writer -> never reaches the tracker
        assert await wait_until(lambda: len(h.writer.records) == 2)
        h.writer.accept = True
        ws.feed(depth_frame(9, 12))  # tracker expects U == 6 -> a gap
        assert await wait_until(lambda: gaps, 3)

    h, _ = drive(tmp_path, depth_spec(), [ws], scenario)
    assert gaps and any(k == "GAP_DETECTED" for k, _ in events)
    assert h.env.trackers["spot:btcusdt@depth@100ms"].gen == 1


def test_stop_ends_the_read_loop_promptly_and_closes_the_generation(tmp_path):
    ws = FakeWs()

    async def scenario(h):
        ws.feed(trade_frame(1))
        assert await wait_until(lambda: h.writer.records)

    t0 = time.monotonic()
    h, _ = drive(tmp_path, trade_spec(max_gap=5, first=5), [ws], scenario)
    assert time.monotonic() - t0 < 3
    assert h.of("WS_CLOSE")[-1]["reason"] == "stop" and h.mgr.state == "stopped"


def test_an_unexpected_read_error_is_a_read_error_not_a_connect_failure(tmp_path):
    ws1, ws2 = FakeWs(), FakeWs()

    async def scenario(h):
        ws1.feed(trade_frame(1))
        assert await wait_until(lambda: h.writer.records)
        ws1.q.put_nowait(RuntimeError("boom"))
        assert await wait_until(lambda: len(h.of("WS_OPEN")) == 2, 5)

    h, _ = drive(tmp_path, trade_spec(max_gap=5, first=5), [ws1, ws2], scenario)
    assert h.of("WS_CLOSE")[0]["reason"] == "read_error:RuntimeError"
    assert not h.of("WS_CONNECT_FAIL")


def test_snapshot_view_reports_stream_health(tmp_path):
    ws = FakeWs()

    async def scenario(h):
        ws.feed(trade_frame(1), trade_frame(2))
        assert await wait_until(lambda: len(h.writer.records) == 2)

    h, _ = drive(tmp_path, trade_spec(max_gap=5, first=5), [ws], scenario)
    snap = h.mgr.snapshot()
    assert snap["gen"] == 1 and snap["frames"] == 2
    st = snap["streams"]["spot:btcusdt@trade"]
    assert st["frames"] == 2 and st["healthy"] is True and st["last_frame_age_s"] is not None


def test_a_failure_while_closing_a_good_connection_is_not_reported_as_a_connect_failure(tmp_path):
    class BadExit(FakeWs):
        async def __aexit__(self, *exc):
            raise OSError("close failed")

    ws1, ws2 = BadExit(), FakeWs()

    async def scenario(h):
        ws1.feed(trade_frame(1))
        assert await wait_until(lambda: h.writer.records)
        ws1.server_close()
        assert await wait_until(lambda: len(h.of("WS_OPEN")) == 2, 5)

    h, _ = drive(tmp_path, trade_spec(max_gap=5, first=5), [ws1, ws2], scenario)
    assert not h.of("WS_CONNECT_FAIL")
    assert h.of("WS_CLOSE")[0]["reason"].startswith("closed")


@pytest.mark.parametrize("mode", ["gap", "no_data_flag"])
def test_new_generation_resets_liveness_state(tmp_path, mode):
    ws1, ws2 = FakeWs(), FakeWs()

    async def scenario(h):
        ws1.feed(trade_frame(1))
        assert await wait_until(lambda: h.writer.records)
        assert h.mgr.request_reconnect("gap:x")
        ws2.feed(trade_frame(50))
        assert await wait_until(lambda: len(h.writer.records) == 2, 3)

    h, _ = drive(tmp_path, trade_spec(max_gap=5, first=5), [ws1, ws2], scenario)
    snap = h.mgr.snapshot()
    assert snap["gen"] == 2 and snap["streams"]["spot:btcusdt@trade"]["frames"] == 1
