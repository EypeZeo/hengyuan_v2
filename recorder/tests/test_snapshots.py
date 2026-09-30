from __future__ import annotations

import asyncio
import json

import pytest
from lake import snapshot_L
from test_bridge import SPOT_EV, SPOT_L

from hy_recorder.clock import FakeClock
from hy_recorder.depthtrack import DepthTracker
from hy_recorder.ratelimit import RestGovernor
from hy_recorder.rest import RestError, RestResult
from hy_recorder.snapshots import BACKOFF_CAP_S, SnapshotScheduler
from hy_recorder.state import StateFile

STREAM = "spot:btcusdt@depth@100ms"


def snap(L: int, status: int = 200, headers: dict | None = None) -> RestResult:
    body = json.dumps({"lastUpdateId": L, "bids": [], "asks": []}).encode()
    return RestResult(
        status, headers or {"x-mbx-used-weight-1m": "50"}, body if status == 200 else b"{}", 1, 2, 3
    )


class Harness:
    def __init__(self, tmp_path, *, budget=0.05, keyframe_s=3600.0):
        self.clock = FakeClock()
        state = StateFile(tmp_path)
        state.load()
        self.events: list[tuple[str, dict]] = []
        emit = lambda kind, **f: self.events.append((kind, f))  # noqa: E731
        self.gov = RestGovernor(self.clock, state, emit, budget_fraction=budget)
        self.script: list = []
        self.calls = 0
        self.sunk: list[tuple[str, str, int, bytes]] = []
        self.seq = 5000
        self.sched = SnapshotScheduler(
            clock=self.clock,
            governor=self.gov,
            fetch=self._fetch,
            sink=self._sink,
            tracker_for=lambda v, s: self.tracker,
            emit=emit,
            targets=(("spot", "BTCUSDT"),),
            limit=1000,
            cooldown_s=5.0,
            keyframe_interval_s=keyframe_s,
        )
        self.tracker = DepthTracker(
            "spot",
            "BTCUSDT",
            STREAM,
            emit,
            on_first_event=lambda g: self.sched.first_event("spot", "BTCUSDT", g),
            on_bridged=lambda g: self.sched.bridged("spot", "BTCUSDT", g),
            on_gap=lambda rule: None,
        )

    async def _fetch(self, venue, endpoint, params):
        self.calls += 1
        item = self.script.pop(0)
        if callable(item):
            item = await item()
        if isinstance(item, BaseException):
            raise item
        return item

    def _sink(self, venue, symbol, gen, body):
        self.seq += 1
        self.sunk.append((venue, symbol, gen, body))
        return self.seq

    def start_generation(self, gen):
        self.tracker.new_generation(gen)
        self.sched.generation_started("spot", "BTCUSDT", gen)

    def feed(self, start, stop):
        for ev in SPOT_EV[start:stop]:
            self.tracker.on_event(ev.q, ev.U, ev.u, None, 1_000 + ev.q)

    async def settle(self):
        await self.sched.tick()
        await self.sched.wait_idle()

    def kinds(self):
        return [k for k, _ in self.events]

    def of(self, kind):
        return [f for k, f in self.events if k == kind]


def run(coro):
    return asyncio.run(coro)


def test_generation_start_snapshot_bridges_and_is_recorded(tmp_path):
    async def go():
        h = Harness(tmp_path)
        h.start_generation(1)
        h.feed(0, 5)  # first event arrives -> the scheduler is told; nothing is fetched by the reader
        assert h.calls == 0
        h.script = [snap(SPOT_L)]
        await h.settle()
        assert h.calls == 1 and h.sunk[0][2] == 1
        assert "SNAPSHOT" in h.kinds() and "BRIDGE_OK" in h.kinds()
        assert h.sched.state_of("spot", "BTCUSDT")["pending"] is False
        assert (
            h.of("SNAPSHOT")[0]["trigger"] == "generation_start"
            and h.of("SNAPSHOT")[0]["last_update_id"] == SPOT_L
        )

    run(go())


def test_stale_snapshot_is_retried_after_the_five_second_hard_cooldown_without_growing_backoff(tmp_path):
    async def go():
        h = Harness(tmp_path)
        h.start_generation(1)
        h.feed(0, 5)
        h.script = [snap(SPOT_EV[0].U - 10), snap(SPOT_L)]
        await h.settle()
        assert "SNAPSHOT_STALE" in h.kinds() and h.calls == 1
        assert h.sched.state_of("spot", "BTCUSDT")["backoff_s"] == 0
        h.clock.advance(4.9)
        await h.settle()
        assert h.calls == 1  # cooldown not over yet
        h.clock.advance(0.2)
        await h.settle()
        assert h.calls == 2 and "BRIDGE_OK" in h.kinds()

    run(go())


def test_failures_back_off_exponentially_with_no_attempt_cap_until_bridged(tmp_path):
    async def go():
        h = Harness(tmp_path)
        h.start_generation(1)
        h.feed(0, 5)
        h.script = [RestError("timeout") for _ in range(12)] + [snap(SPOT_L)]
        backoffs = []
        for i in range(13):
            await h.settle()
            assert h.calls == i + 1, "an attempt must happen every time it is due (no cap)"
            if i < 12:
                b = h.sched.state_of("spot", "BTCUSDT")["backoff_s"]
                backoffs.append(b)
                h.clock.advance(b + 0.1)
        assert backoffs == [5, 10, 20, 40, 80, 160, 300, 300, 300, 300, 300, 300]
        assert max(backoffs) == BACKOFF_CAP_S
        assert "BRIDGE_OK" in h.kinds() and h.sched.state_of("spot", "BTCUSDT")["pending"] is False
        assert [f["backoff_s"] for f in h.of("SNAPSHOT_FAIL")][:3] == [5, 10, 20]

    run(go())


def test_reconnect_recovers_within_a_minute_and_never_waits_for_the_hourly_keyframe(tmp_path):
    """The 10:05 scenario: a reconnect, three failed snapshot attempts, then success."""

    async def go():
        h = Harness(tmp_path)
        h.start_generation(1)
        h.feed(0, 5)
        h.script = [snap(SPOT_L)]
        await h.settle()
        assert "BRIDGE_OK" in h.kinds()
        h.clock.advance(1500)  # stable for a while
        t_reconnect = h.clock.mono_s()
        h.events.clear()
        h.start_generation(2)  # reconnect: brand new generation
        h.feed(0, 5)
        h.script = [RestError("timeout"), RestError("timeout"), snap(500), snap(SPOT_L)]
        while "BRIDGE_OK" not in h.kinds():
            await h.settle()
            h.clock.advance(1.0)
            assert h.clock.mono_s() - t_reconnect < 120, "must not wait for the next hourly snapshot"
        assert h.clock.mono_s() - t_reconnect < 60
        assert [f["gen"] for f in h.of("BRIDGE_OK")] == [2]
        assert all(f["trigger"] == "generation_start" for f in h.of("SNAPSHOT"))

    run(go())


def test_429_during_bridging_pauses_all_rest_for_the_deep_sleep_then_recovers(tmp_path):
    async def go():
        h = Harness(tmp_path)
        h.start_generation(1)
        h.feed(0, 5)
        h.script = [snap(0, status=429, headers={"retry-after": "10"}), snap(SPOT_L)]
        await h.settle()
        assert h.calls == 1 and h.of("RATE_LIMIT")[0]["sleep_s"] == 300
        for _ in range(60):  # 5 minutes minus a bit: many ticks, zero REST calls
            h.clock.advance(4.9)
            await h.settle()
        assert h.calls == 1, "no REST during the deep sleep"
        assert h.of("SNAPSHOT_DEFERRED") and len(h.of("SNAPSHOT_DEFERRED")) < 10  # rate-limited ledger noise
        h.clock.advance(20)
        await h.settle()
        assert h.calls == 2 and "BRIDGE_OK" in h.kinds()

    run(go())


def test_snapshot_completing_after_a_reconnect_is_discarded_not_attached_to_the_new_generation(tmp_path):
    async def go():
        h = Harness(tmp_path)
        h.start_generation(1)
        h.feed(0, 5)
        release = asyncio.Event()

        async def slow():
            await release.wait()
            return snap(SPOT_L)

        h.script = [slow]
        await h.sched.tick()  # request goes in flight
        await asyncio.sleep(0)
        assert h.sched.state_of("spot", "BTCUSDT")["inflight"]
        h.start_generation(2)  # the connection was replaced while the request was in flight
        release.set()
        await h.sched.wait_idle()
        assert h.of("SNAPSHOT_DISCARDED") and h.sunk == []
        assert h.tracker.gen == 2 and "BRIDGE_OK" not in h.kinds()

    run(go())


def test_keyframe_is_hourly_skippable_and_never_blocks_bridging(tmp_path):
    async def go():
        h = Harness(tmp_path, keyframe_s=3600.0)
        h.start_generation(1)
        h.feed(0, 30)
        h.script = [snap(SPOT_L)]
        await h.settle()
        assert "BRIDGE_OK" in h.kinds()
        h.clock.advance(3599)
        await h.settle()
        assert h.calls == 1
        h.clock.advance(2)
        h.script = [snap(SPOT_EV[20].u - 1)]
        await h.settle()
        assert h.calls == 2 and h.of("SNAPSHOT")[-1]["trigger"] == "keyframe"
        # the next keyframe is an hour later; while REST is asleep a due keyframe is skipped, not caught up
        h.clock.advance(3601)
        h.gov.on_response("spot", 429, {})
        await h.settle()
        assert h.calls == 2

    run(go())


def test_budget_denial_defers_without_burning_the_backoff(tmp_path):
    async def go():
        h = Harness(tmp_path, budget=0.01)  # 1% of 6000 = 60/min: one 50-weight snapshot only
        h.start_generation(1)
        h.feed(0, 5)
        h.script = [snap(SPOT_EV[0].U - 10), snap(SPOT_L)]
        await h.settle()  # first (stale) one is allowed
        h.clock.advance(5.1)
        await h.settle()  # second is over budget -> deferred
        assert h.calls == 1 and h.of("SNAPSHOT_DEFERRED")[0]["reason"] == "budget"
        assert h.sched.state_of("spot", "BTCUSDT")["backoff_s"] == 0
        h.clock.advance(61)
        await h.settle()
        assert h.calls == 2 and "BRIDGE_OK" in h.kinds()

    run(go())


def test_forbidden_or_dropped_snapshot_never_retries_hot(tmp_path):
    async def go():
        h = Harness(tmp_path)
        h.start_generation(1)
        h.feed(0, 5)
        h.sched._sink = lambda *a: None  # the writer queue refused the record
        h.script = [snap(SPOT_L), snap(SPOT_L)]
        await h.settle()
        assert (
            h.of("SNAPSHOT_FAIL")[0]["reason"] == "dropped"
            and h.sched.state_of("spot", "BTCUSDT")["backoff_s"] == 5
        )
        await h.settle()  # immediately again: still inside the backoff
        assert h.calls == 1

    run(go())


@pytest.mark.parametrize("bad", [b"not json", b"{}", b'{"lastUpdateId":"x"}'])
def test_bad_bodies_count_as_failures(tmp_path, bad):
    async def go():
        h = Harness(tmp_path)
        h.start_generation(1)
        h.feed(0, 5)
        h.script = [RestResult(200, {}, bad, 1, 2, 3)]
        await h.settle()
        assert h.of("SNAPSHOT_FAIL")[0]["reason"] == "bad_body"

    run(go())


def test_fixture_snapshot_L_matches_the_bridge_tests():
    assert snapshot_L("spot") == SPOT_L
