from __future__ import annotations

import json

import pytest
from conftest import FIXTURES, load_frames

from hy_recorder.bridge import BridgeMachine, BridgeState, EventRef


def events(name: str, venue: str) -> list[EventRef]:
    out = []
    for i, (_, _, payload) in enumerate(load_frames(name)):
        d = json.loads(payload)["data"]
        out.append(EventRef(q=i + 1, U=d["U"], u=d["u"], pu=d.get("pu")))
    return out


def snapshot_L(name: str) -> int:
    return json.loads((FIXTURES / name).read_bytes())["lastUpdateId"]


SPOT_EV = events("spot_depth_bridge_frames.jsonl", "spot")
SPOT_L = snapshot_L("spot_depth_snapshot_l100.json")
USDM_EV = events("usdm_depth_bridge_frames.jsonl", "usdm")
USDM_L = snapshot_L("usdm_depth_snapshot_l100.json")


def run(venue, evs, L, snap_after: int | None, *, snap_q: int = 900):
    """Feed events; deliver the snapshot after ``snap_after`` events (None = never, 0 = before all)."""
    m = BridgeMachine(venue)
    trs = []
    if snap_after == 0:
        trs += m.on_snapshot(L, snap_q)
    for i, ev in enumerate(evs, 1):
        trs += m.on_event(ev)
        if snap_after == i:
            trs += m.on_snapshot(L, snap_q)
    return m, trs


def kinds(trs):
    return [t.kind for t in trs]


def test_real_data_shape_matches_the_documented_rules():
    # spot: the bridging event is the first with u > L and U <= L+1 (here U == L+1 exactly)
    cand = next(e for e in SPOT_EV if e.u > SPOT_L)
    assert cand.U <= SPOT_L + 1 <= cand.u
    # usdm: first event with u >= L must have U <= L
    cand = next(e for e in USDM_EV if e.u >= USDM_L)
    assert cand.U <= USDM_L <= cand.u
    # whole fixture chains (real data): spot U == prev u + 1, usdm pu == prev u
    assert all(b.U == a.u + 1 for a, b in zip(SPOT_EV, SPOT_EV[1:], strict=False))
    assert all(b.pu == a.u for a, b in zip(USDM_EV, USDM_EV[1:], strict=False))


@pytest.mark.parametrize("snap_after", [0, 1, 2, 3, 10, 30])
def test_spot_bridges_regardless_of_snapshot_arrival_time(snap_after):
    m, trs = run("spot", SPOT_EV, SPOT_L, snap_after)
    assert m.state is BridgeState.VERIFIED
    assert kinds(trs).count("BRIDGED") == 1 and "GAP" not in kinds(trs)
    bridged = next(t for t in trs if t.kind == "BRIDGED")
    assert bridged.detail["L"] == SPOT_L and bridged.detail["U"] <= SPOT_L + 1 <= bridged.detail["u"]
    assert m.prev_u == SPOT_EV[-1].u


@pytest.mark.parametrize("snap_after", [0, 1, 2, 3, 10, 30])
def test_usdm_bridges_regardless_of_snapshot_arrival_time(snap_after):
    m, trs = run("usdm", USDM_EV, USDM_L, snap_after)
    assert m.state is BridgeState.VERIFIED
    assert kinds(trs).count("BRIDGED") == 1 and "GAP" not in kinds(trs)
    assert m.prev_u == USDM_EV[-1].u


def test_no_snapshot_stays_buffering_and_is_never_verified():
    m, trs = run("spot", SPOT_EV, SPOT_L, None)
    assert m.state is BridgeState.BUFFERING and "BRIDGED" not in kinds(trs)


def test_stale_snapshot_is_reported_and_a_fresh_one_bridges():
    m, _ = run("spot", SPOT_EV, SPOT_L, 5)  # bridged already
    m = BridgeMachine("spot")
    for ev in SPOT_EV[:5]:
        m.on_event(ev)
    stale = m.on_snapshot(SPOT_EV[0].U - 10, 800)  # older than the first buffered U
    assert kinds(stale) == ["SNAPSHOT_STALE"] and m.state is BridgeState.BUFFERING
    fresh = m.on_snapshot(SPOT_L, 801)
    assert kinds(fresh)[0] == "BRIDGED" and m.state is BridgeState.VERIFIED


def test_snapshot_ahead_of_buffer_waits_in_bridging_state():
    m = BridgeMachine("spot")
    m.on_event(SPOT_EV[0])
    trs = m.on_snapshot(SPOT_EV[10].u + 5, 800)  # newer than everything seen so far
    assert trs == [] and m.state is BridgeState.BRIDGING
    later = []
    for ev in SPOT_EV[1:]:
        later += m.on_event(ev)
    assert m.state is BridgeState.VERIFIED  # events caught up with the snapshot, so it bridged
    assert kinds(later).count("BRIDGED") == 1


def test_gap_in_chain_is_terminal_and_not_backfilled_by_a_later_snapshot():
    for venue, evs, L in (("spot", SPOT_EV, SPOT_L), ("usdm", USDM_EV, USDM_L)):
        broken = [e for i, e in enumerate(evs) if i != 12]  # drop one real event
        m, trs = run(venue, broken, L, 3)
        assert m.state is BridgeState.GAP
        gap = next(t for t in trs if t.kind == "GAP")
        assert gap.detail["got"] != gap.detail["expected"]
        # a later snapshot must NOT re-open the generation
        assert m.on_snapshot(evs[-1].u, 999) == [] and m.state is BridgeState.GAP
        assert m.on_event(evs[-1]) == []
        m.reset()  # only a new generation (reconnect) starts over
        assert m.state is BridgeState.UNVERIFIED


def test_spot_duplicate_event_is_an_overlap_not_a_gap():
    evs = list(SPOT_EV[:20])
    evs.insert(10, evs[9])  # duplicate delivery
    m, trs = run("spot", evs, SPOT_L, 3)
    assert m.state is BridgeState.VERIFIED and "OVERLAP" in kinds(trs) and "GAP" not in kinds(trs)


def test_keyframe_snapshot_inside_verified_chain_is_covered():
    m, _ = run("spot", SPOT_EV, SPOT_L, 3)
    mid = SPOT_EV[20]
    kf = m.on_snapshot(mid.u - 1, 950)  # a snapshot taken mid-stream
    assert kinds(kf) == ["KEYFRAME"] and kf[0].detail["covered"] is True
    far = m.on_snapshot(SPOT_EV[-1].u + 10_000, 951)
    assert far[0].detail["covered"] is False  # beyond anything we hold


def test_snapshot_before_any_event_is_kept_for_the_first_event():
    m = BridgeMachine("usdm")
    assert m.on_snapshot(USDM_L, 1) == [] and m.state is BridgeState.UNVERIFIED
    trs = []
    for ev in USDM_EV:
        trs += m.on_event(ev)
    assert m.state is BridgeState.VERIFIED and kinds(trs).count("BRIDGED") == 1


def test_bad_venue_rejected():
    with pytest.raises(ValueError):
        BridgeMachine("coinm")
