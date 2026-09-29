from __future__ import annotations

import json

import pytest
from lake import SPOT_DEPTH, SPOT_TRADE, LakeBuilder

from hy_recorder.cli import main
from hy_recorder.report import build_report, render_text


def trade_with_offset(b: LakeBuilder, t_id: int, offset_ms: float) -> bytes:
    """A trade whose event time E is ``offset_ms`` before the host receive time."""
    e_ms = int(b.clock.wall_us() / 1000 - offset_ms)
    return json.dumps({"stream": "btcusdt@trade", "data": {"e": "trade", "E": e_ms, "t": t_id}}).encode()


@pytest.fixture
def lake(tmp_path):
    b = LakeBuilder(tmp_path)
    gen = b.open_conn("spot_depth_btcusdt", [SPOT_DEPTH])
    b.clock.advance(3.2)
    b.event("BRIDGE_OK", stream=SPOT_DEPTH, gen=gen, bridge_q=5, L=100, U=99, u=101)
    tgen = b.open_conn("spot_trade", [SPOT_TRADE])
    for i in range(1, 41):
        b.record("spot", "trade", SPOT_TRADE, trade_with_offset(b, i, 5.0), tgen, dt_us=100_000)
    b.event("CLOCK_PROBE", venue="spot", server_ms=1, rtt_us=140_000, offset_ms=-2.5, start_wall_us=1)
    b.event("CLOCK_PROBE", venue="spot", server_ms=1, rtt_us=160_000, offset_ms=-3.5, start_wall_us=1)
    b.event(
        "GAP_DETECTED", stream=SPOT_DEPTH, gen=gen, bad_q=9, rule="spot_U_gt_prev_u_plus_1", expected=1, got=2
    )
    b.event(
        "WS_CLOSE",
        conn="spot_depth_btcusdt",
        gen=gen,
        reason="gap:spot_U_gt_prev_u_plus_1",
        frames=3,
        duration_s=1.0,
    )
    b.event(
        "RATE_LIMIT", venue="spot", status=429, retry_after_s=0.0, sleep_s=300.0, level=0, used_weight=None
    )
    b.event("STREAM_STALL", conn="spot_trade", gen=tgen, stream=SPOT_TRADE, silent_s=9.0, max_gap_s=5.0)
    b.stop()
    return tmp_path


def test_report_summarises_a_lake(lake):
    rep = build_report(lake, sample_every=1)
    assert rep["runs"] == [1] and rep["unclean_runs"] == []
    trade = rep["streams"][SPOT_TRADE]
    assert trade["records"] == 40
    d = trade["recv_minus_event_ms"]
    assert d["n"] == 40 and abs(d["p50"] - 5.0) < 0.01 and abs(d["min"] - 5.0) < 0.01
    assert trade["per_second"] == pytest.approx(10.0, rel=0.05)
    bridge = rep["bridge_seconds_from_ws_open"]
    assert bridge["n"] == 1 and bridge["p50"] == pytest.approx(3.2, abs=0.01)
    assert rep["slow_bridges_over_10s"] == []
    c = rep["connections"]
    assert c["opens_per_connection"] == {"spot_depth_btcusdt": 1, "spot_trade": 1}
    assert c["close_reasons"] == {"gap": 1} and c["stalls"] == 1
    assert rep["gaps"][0]["rule"] == "spot_U_gt_prev_u_plus_1"
    assert rep["rate_limits"]["http_429_403_451_events"] == 1 and rep["rate_limits"]["http_418_bans"] == 0
    probes = rep["clock_probe"]
    assert probes["n"] == 2 and probes["offset_ms"]["min"] == -3.5 and probes["rtt_ms"]["max"] == 160.0
    assert rep["storage"]["sealed_segments"] >= 1 and rep["storage"]["sealed_bytes"] > 0
    assert rep["storage"]["mb_per_day"] is None, "a few seconds of data must not be extrapolated to a day"


def test_slow_bridges_are_listed(tmp_path):
    b = LakeBuilder(tmp_path)
    gen = b.open_conn("usdm_depth_btcusdt", ["usdm:btcusdt@depth@100ms"])
    b.clock.advance(42.0)
    b.event("BRIDGE_OK", stream="usdm:btcusdt@depth@100ms", gen=gen, bridge_q=3, L=1, U=1, u=2)
    b.stop()
    rep = build_report(tmp_path)
    assert rep["slow_bridges_over_10s"] == [{"stream": "usdm:btcusdt@depth@100ms", "gen": 1, "seconds": 42.0}]


def test_generations_restart_each_run_and_must_not_be_mixed_up(tmp_path):
    b1 = LakeBuilder(tmp_path)
    g = b1.open_conn("spot_depth_btcusdt", [SPOT_DEPTH])
    b1.clock.advance(1.0)
    b1.event("BRIDGE_OK", stream=SPOT_DEPTH, gen=g, bridge_q=2, L=1, U=1, u=2)
    b1.stop()
    b2 = LakeBuilder(tmp_path)  # run 2 also opens generation 1
    b2.clock.advance(86_400)
    g2 = b2.open_conn("spot_depth_btcusdt", [SPOT_DEPTH])
    b2.clock.advance(8.0)
    b2.event("BRIDGE_OK", stream=SPOT_DEPTH, gen=g2, bridge_q=2, L=1, U=1, u=2)
    b2.stop()
    rep = build_report(tmp_path)
    assert rep["runs"] == [1, 2]
    secs = sorted([1.0, 8.0])
    assert (
        rep["bridge_seconds_from_ws_open"]["min"] == secs[0]
        and rep["bridge_seconds_from_ws_open"]["max"] == secs[1]
    )


def test_an_unclean_run_is_reported(tmp_path):
    b = LakeBuilder(tmp_path)
    gen = b.open_conn("spot_trade", [SPOT_TRADE])
    for i in range(1, 5):
        b.record("spot", "trade", SPOT_TRADE, trade_with_offset(b, i, 1.0), gen)
    b.crash()
    assert build_report(tmp_path)["unclean_runs"] == [1]


def test_mb_per_day_is_extrapolated_only_from_a_long_enough_span(tmp_path):
    import os

    b = LakeBuilder(tmp_path)
    gen = b.open_conn("spot_trade", [SPOT_TRADE])
    for hour in range(4):
        for i in range(300):
            payload = json.dumps(
                {
                    "stream": "btcusdt@trade",
                    "data": {"e": "trade", "t": hour * 300 + i, "pad": os.urandom(200).hex()},
                }
            ).encode()
            b.record("spot", "trade", SPOT_TRADE, payload, gen, dt_us=1000)
        b.clock.advance(3600)
    b.stop()
    rep = build_report(tmp_path)
    assert rep["storage"]["span_hours"] > 2.9 and rep["storage"]["mb_per_day"] > 1.0


def test_text_and_cli_outputs(lake, capsys):
    text = render_text(build_report(lake, sample_every=1))
    assert "storage:" in text and SPOT_TRADE in text and "rate limits:" in text and "bridge seconds" in text
    assert main(["report", "--root", str(lake), "--sample-every", "1"]) == 0
    assert "streams:" in capsys.readouterr().out
    assert main(["report", "--root", str(lake), "--json"]) == 0
    assert json.loads(capsys.readouterr().out)["runs"] == [1]


def test_report_on_an_empty_directory_does_not_crash(tmp_path):
    rep = build_report(tmp_path)
    assert rep["streams"] == {} and rep["storage"]["sealed_segments"] == 0 and rep["runs"] == []
    assert isinstance(render_text(rep), str)
