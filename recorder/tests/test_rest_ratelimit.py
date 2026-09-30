from __future__ import annotations

import http.client

import pytest

from hy_recorder.clock import FakeClock
from hy_recorder.guard import ForbiddenEndpoint
from hy_recorder.ratelimit import SLEEP_BASE_S, SLEEP_CAP_S, RestGovernor
from hy_recorder.rest import MAX_BODY, RestClient, RestError
from hy_recorder.state import StateFile


# -- REST client with fake connections -----------------------------------------------------------
class FakeResponse:
    def __init__(self, status=200, headers=None, body=b"{}", will_close=False):
        self.status, self._headers, self._body, self.will_close = status, headers or [], body, will_close

    def getheaders(self):
        return self._headers

    def read(self, n=-1):
        return self._body if n < 0 else self._body[:n]


class FakeConn:
    def __init__(self, script):
        self.script, self.requests, self.closed = list(script), [], False

    def request(self, method, path, headers=None):
        self.requests.append((method, path, dict(headers or {})))

    def getresponse(self):
        item = self.script.pop(0)
        if isinstance(item, BaseException):
            raise item
        return item

    def close(self):
        self.closed = True


def client_with(*conns):
    made = []
    pool = list(conns)

    def factory(host):
        conn = pool.pop(0)
        made.append((host, conn))
        return conn

    return RestClient(conn_factory=factory, clock=FakeClock()), made


def test_get_sends_only_get_with_minimal_headers_and_lowercases_response_headers():
    conn = FakeConn(
        [
            FakeResponse(
                200, [("X-MBX-USED-WEIGHT-1M", "55"), ("Content-Type", "application/json")], b'{"a":1}'
            )
        ]
    )
    rest, made = client_with(conn)
    res = rest.get("spot", "depth", {"symbol": "BTCUSDT", "limit": 100})
    assert made[0][0] == "api.binance.com"
    method, path, headers = conn.requests[0]
    assert method == "GET" and path == "/api/v3/depth?limit=100&symbol=BTCUSDT"
    assert set(headers) == {"User-Agent", "Accept"}
    assert res.status == 200 and res.body == b'{"a":1}' and res.headers["x-mbx-used-weight-1m"] == "55"
    assert res.end_mono_us >= res.start_mono_us


def test_keep_alive_reuses_the_connection_and_close_header_drops_it():
    conn = FakeConn([FakeResponse(), FakeResponse(headers=[("Connection", "close")]), FakeResponse()])
    conn2 = FakeConn([FakeResponse()])
    rest, made = client_with(conn, conn2)
    rest.get("usdm", "time")
    rest.get("usdm", "time")  # same connection
    assert len(made) == 1
    rest.get("usdm", "time")  # the previous response said close -> a new connection is needed
    assert len(made) == 2 and conn.closed


def test_stale_keep_alive_is_retried_once_on_a_fresh_connection():
    stale = FakeConn([http.client.RemoteDisconnected("closed")])
    fresh = FakeConn([FakeResponse(200, [], b"ok")])
    rest, made = client_with(stale, fresh)
    assert rest.get("spot", "time").body == b"ok" and len(made) == 2 and stale.closed
    dead1, dead2 = FakeConn([ConnectionResetError()]), FakeConn([ConnectionResetError()])
    rest2, _ = client_with(dead1, dead2)
    with pytest.raises(RestError) as exc:
        rest2.get("spot", "time")
    assert exc.value.kind == "transport"


def test_non_200_is_a_result_not_an_exception():
    rest, _ = client_with(FakeConn([FakeResponse(429, [("Retry-After", "7")], b"{}")]))
    res = rest.get("spot", "time")
    assert res.status == 429 and res.headers["retry-after"] == "7"


def test_oversized_body_and_forbidden_endpoint():
    rest, _ = client_with(FakeConn([FakeResponse(200, [], b"x" * (MAX_BODY + 5))]))
    with pytest.raises(RestError) as exc:
        rest.get("spot", "time")
    assert exc.value.kind == "too_large"
    untouched, made = client_with()
    with pytest.raises(ForbiddenEndpoint):
        untouched.get("spot", "order", {})
    assert made == []  # rejected before any connection was created


# -- governor ------------------------------------------------------------------------------------
@pytest.fixture
def gov(tmp_path):
    clock = FakeClock()
    state = StateFile(tmp_path)
    state.load()
    events: list[tuple[str, dict]] = []
    g = RestGovernor(clock, state, lambda kind, **f: events.append((kind, f)))
    g.clock, g.state, g.events = clock, state, events  # convenience handles for the tests
    return g


def test_budget_caps_this_process_at_five_percent_of_the_limit(gov):
    # spot: 5% of 6000 = 300 per minute -> six 50-weight snapshots fit, the seventh does not
    for _ in range(6):
        assert gov.permit("spot", 50).ok
        gov.spent("spot", 50)
    denied = gov.permit("spot", 50)
    assert not denied.ok and denied.reason == "budget" and 0 < denied.retry_after_s <= 60
    gov.clock.advance(61)
    assert gov.permit("spot", 50).ok
    # usdm: 5% of 2400 = 120 -> six 20-weight snapshots
    for _ in range(6):
        assert gov.permit("usdm", 20).ok
        gov.spent("usdm", 20)
    assert not gov.permit("usdm", 20).ok


def test_shared_address_pressure_pauses_rest_even_when_our_own_use_is_tiny(gov):
    assert gov.on_response("spot", 200, {"x-mbx-used-weight-1m": "3200"}) == "ok"
    p = gov.permit("spot", 5)
    assert not p.ok and p.reason == "shared_pressure"
    assert gov.permit("usdm", 5).ok  # the other venue has its own address limit
    gov.clock.advance(61)  # the observation is stale now
    assert gov.permit("spot", 5).ok


def test_429_starts_a_five_minute_deep_sleep_and_a_repeat_doubles_it(gov):
    assert gov.on_response("spot", 429, {"retry-after": "10"}) == "sleep"
    assert gov.events[-1][0] == "RATE_LIMIT" and gov.events[-1][1]["sleep_s"] == SLEEP_BASE_S
    p = gov.permit("spot", 5)
    assert not p.ok and p.reason == "deep_sleep" and 299 < p.retry_after_s <= 300
    assert gov.permit("usdm", 1).reason == "deep_sleep"  # all REST, every venue
    gov.clock.advance(299)
    assert not gov.permit("spot", 5).ok
    gov.clock.advance(2)
    assert gov.permit("spot", 5).ok
    # a second limit response within the hour doubles the sleep, then again, up to the cap
    for want in (600, 1200, 2400, 4800, 7200, 7200):
        gov.on_response("spot", 429, {})
        assert gov.events[-1][1]["sleep_s"] == min(SLEEP_CAP_S, want)
        gov.clock.advance(want + 1)
    # an hour of quiet resets the level
    gov.clock.advance(3700)
    gov.on_response("spot", 429, {})
    assert gov.events[-1][1]["sleep_s"] == SLEEP_BASE_S


def test_a_late_429_during_a_sleep_neither_escalates_nor_shortens_it(gov):
    gov.on_response("spot", 429, {})
    before = gov.sleep_remaining_s()
    gov.clock.advance(10)
    gov.on_response("usdm", 429, {"retry-after": "1"})  # an in-flight request completing during the sleep
    ev = gov.events[-1]
    assert ev[1]["during_sleep"] is True and ev[1]["level"] == 0
    assert abs(gov.sleep_remaining_s() - (before - 10)) < 1
    gov.clock.advance(301)
    gov.on_response("spot", 429, {})  # a genuine second violation right after the sleep: level 1
    assert gov.events[-1][1]["sleep_s"] == 600


def test_retry_after_longer_than_five_minutes_wins(gov):
    gov.on_response("spot", 429, {"retry-after": "900"})
    assert gov.events[-1][1]["sleep_s"] == 900
    assert gov.sleep_remaining_s() > 890


def test_deep_sleep_survives_a_restart_and_a_wall_clock_step_back(gov, tmp_path):
    gov.on_response("spot", 429, {})
    # restart: a new governor (new StateFile object, same directory) still sleeps
    state2 = StateFile(tmp_path)
    state2.load()
    g2 = RestGovernor(gov.clock, state2, lambda *a, **k: None)
    assert g2.sleep_remaining_s() > 290 and not g2.permit("spot", 5).ok
    # within one process a backwards wall-clock step cannot shorten the sleep (or lengthen: max of both)
    gov.clock.jump_wall(-10_000)
    assert gov.sleep_remaining_s() > 290
    gov.clock.jump_wall(+10_000)
    gov.clock.advance(301)
    assert gov.permit("spot", 5).ok


def test_418_freezes_all_binance_traffic_and_is_persistent(gov, tmp_path):
    assert gov.on_response("usdm", 418, {"retry-after": "0"}) == "ban"
    ev = gov.events[-1]
    assert ev[0] == "BAN" and ev[1]["ban_s"] == 120  # never shorter than the 2-minute minimum
    assert gov.permit("spot", 1).reason == "ban" and gov.ws_blocked_s() > 100
    state2 = StateFile(tmp_path)
    state2.load()
    assert RestGovernor(gov.clock, state2, lambda *a, **k: None).ban_remaining_s() > 100
    gov.clock.advance(121)
    assert gov.permit("spot", 1).ok and gov.ws_blocked_s() == 0
    gov.on_response("spot", 418, {"retry-after": "3600"})
    assert gov.events[-1][1]["ban_s"] == 3600


def test_403_and_451_are_treated_like_429_and_451_is_flagged(gov):
    assert gov.on_response("spot", 403, {}) == "sleep"
    assert gov.events[-1][1]["status"] == 403 and gov.sleep_remaining_s() > 0
    gov.clock.advance(400)
    assert gov.on_response("usdm", 451, {}) == "sleep"
    kinds = [e[0] for e in gov.events]
    assert "GEO_BLOCK_SUSPECT" in kinds and kinds[-1] == "RATE_LIMIT"


def test_websocket_reconnects_are_spaced_while_rest_sleeps(gov):
    assert gov.ws_min_spacing_s() == 0
    gov.on_response("spot", 429, {})
    assert gov.ws_min_spacing_s() == 30 and gov.ws_blocked_s() == 0  # not banned: reconnects still allowed
    gov.clock.advance(301)
    assert gov.ws_min_spacing_s() == 0


def test_snapshot_dict_for_status_file(gov):
    gov.on_response("spot", 200, {"x-mbx-used-weight-1m": "77"})
    gov.on_response("spot", 429, {})
    snap = gov.snapshot()
    assert (
        snap["used_weight_1m"] == {"spot": 77}
        and snap["counters"]["429"] == 1
        and snap["sleep_remaining_s"] > 290
    )
