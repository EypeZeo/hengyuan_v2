from __future__ import annotations

import random

import pytest
from lake import SPOT_TRADE, LakeBuilder, write_simple

from hy_recorder.clock import FakeClock
from hy_recorder.fsutil import US_PER_HOUR
from hy_recorder.manifest import Manifest
from hy_recorder.retention import (
    SegInfo,
    ack_dir,
    ensure_reserve,
    plan_cleanup,
    release_reserve,
    reserve_path,
    retention_steps,
    run_cleanup,
)
from hy_recorder.verify import verify_lake

GB = 1 << 30
NOW = 1_790_691_993_000_000


def seg(name: str, age_h: float, *, acked: bool, size: int = GB) -> SegInfo:
    last = NOW - int(age_h * US_PER_HOUR)
    return SegInfo(name, size, last, "h" + name, acked, last - US_PER_HOUR)


def plan(segs, *, free_gb: float, warn_gb=5.0, floor_gb=2.0, retain=48.0):
    return plan_cleanup(
        segs,
        now_us=NOW,
        free_bytes=int(free_gb * GB),
        warn_bytes=int(warn_gb * GB),
        floor_bytes=int(floor_gb * GB),
        retain_hours=retain,
    )


def test_steps():
    assert retention_steps(48) == (48, 24, 12, 6)
    assert retention_steps(72) == (72, 24, 12, 6)
    assert retention_steps(10) == (10, 6)
    assert retention_steps(6) == (6,)


def test_plenty_of_space_only_removes_acked_and_expired_segments():
    segs = [
        seg("old_acked", 60, acked=True),
        seg("old_unacked", 60, acked=False),
        seg("young_acked", 10, acked=True),
        seg("young_unacked", 3, acked=False),
    ]
    p = plan(segs, free_gb=10)
    assert [a.name for a in p.actions] == ["old_acked"] and p.actions[0].reason == "acked_expired"
    assert p.stepdowns == [] and not p.hard_stop and p.free_after == 11 * GB


def test_pressure_evicts_unacked_expired_segments_oldest_first_and_marks_them():
    segs = [
        seg("u100", 100, acked=False),
        seg("u80", 80, acked=False),
        seg("u60", 60, acked=False),
        seg("u50", 50, acked=False),
        seg("fresh", 5, acked=False),
    ]
    p = plan(segs, free_gb=3.5)  # 1.5 GB below the warning line -> two evictions
    assert [a.name for a in p.actions] == ["u100", "u80"]
    assert all(a.reason == "evicted_unacked" and not a.acked for a in p.actions)
    assert p.stepdowns == [] and p.free_after == int(5.5 * GB)


def test_retention_steps_down_only_as_far_as_needed():
    segs = [
        seg("a30", 30, acked=False),
        seg("a20", 20, acked=False),
        seg("a14", 14, acked=False),
        seg("a8", 8, acked=False),
        seg("a2", 2, acked=False),
    ]
    p = plan(segs, free_gb=4.0)  # nothing is 48 h old; need 1 GB back -> 24 h level frees the 30 h one
    assert [a.name for a in p.actions] == ["a30"] and p.stepdowns == [24.0]
    p = plan(segs, free_gb=1.0)  # needs 4 GB: 24 h -> a30, then 12 h -> a20, a14, then 6 h -> a8
    assert [a.name for a in p.actions] == ["a30", "a20", "a14", "a8"]
    assert p.stepdowns == [24.0, 12.0, 6.0] and p.free_after == 5 * GB and not p.hard_stop
    assert "a2" not in [a.name for a in p.actions]  # data younger than the lowest step is never removed


def test_hard_stop_only_after_everything_older_than_six_hours_is_gone():
    segs = [seg("a30", 30, acked=False), seg("a2", 2, acked=False), seg("a1", 1, acked=False)]
    p = plan(segs, free_gb=0.5)
    assert [a.name for a in p.actions] == ["a30"] and p.hard_stop and p.free_after < 2 * GB
    assert p.stepdowns == [24.0, 12.0, 6.0]


def test_policy_property_never_stop_while_something_can_still_be_cleaned():
    rng = random.Random(7)
    for _ in range(300):
        segs = [
            seg("s%d" % i, rng.uniform(0.5, 200), acked=rng.random() < 0.5, size=rng.randint(1, 3) * GB // 2)
            for i in range(rng.randint(0, 25))
        ]
        free = rng.uniform(0.2, 12.0)
        retain = rng.choice([48.0, 72.0, 24.0])
        p = plan(segs, free_gb=free, retain=retain)
        removed = {a.name for a in p.actions}
        if p.hard_stop:
            leftovers = [
                s
                for s in segs
                if s.name not in removed and (NOW - s.last_t) / US_PER_HOUR >= min(retention_steps(retain))
            ]
            assert leftovers == [], "stopped while cleanable data was left"
        for a in p.actions:  # the age rule is respected for every removed segment
            assert (NOW - a.last_t) / US_PER_HOUR >= a.level_h - 1e-9
            assert a.level_h in retention_steps(retain)
        assert len(removed) == len(p.actions)  # no duplicates


# -- executor on real files ---------------------------------------------------------------------
def test_run_cleanup_deletes_records_prunes_and_stays_verifiable(tmp_path):
    b = LakeBuilder(tmp_path)
    g = b.open_conn("spot_trade", [SPOT_TRADE])
    names: list[str] = []
    for _hour in range(3):  # three hourly segments of real trades
        write_simple(b, "spot", "trade", SPOT_TRADE, "spot_trade_frames.jsonl", g, limit=10)
        b.clock.advance(3600)
    b.stop()
    manifest = Manifest(tmp_path)
    live = manifest.live_segments()
    names = sorted(live)
    assert len(names) == 3
    ack = ack_dir(tmp_path)
    ack.mkdir()
    (ack / live[names[0]]["sha256"]).write_bytes(b"")  # only the oldest was copied off the host
    events: list[tuple[str, dict]] = []
    clock = FakeClock(wall_us=b.clock.wall_us() + 60 * US_PER_HOUR)  # much later: everything is old
    plan_ = run_cleanup(
        tmp_path,
        manifest,
        clock,
        lambda k, **f: events.append((k, f)),
        warn_bytes=1,
        floor_bytes=0,
        retain_hours=48,
        free_bytes=10 * GB,
    )
    assert [a.name for a in plan_.actions] == [names[0]]  # plenty of space: only the acked one goes
    assert not (tmp_path / names[0]).exists() and (tmp_path / names[1]).exists()
    assert [k for k, _ in events] == ["PRUNED_ACKED"]
    prune = [e for e in manifest.read_all() if e.get("ev") == "prune"][0]
    assert prune["sha256"] == live[names[0]]["sha256"] and prune["first"]["q"] < prune["last"]["q"]
    rep = verify_lake(tmp_path)
    assert "MANIFEST_DANGLING" not in {i.code for i in rep.issues}
    assert "SEQ_HOLE" not in {i.code for i in rep.issues}, "the pruned range is accounted for by the manifest"
    assert rep.exit_code == 0, rep.render_text()


def test_run_cleanup_under_pressure_evicts_unacked_with_an_auditable_event(tmp_path):
    b = LakeBuilder(tmp_path)
    g = b.open_conn("spot_trade", [SPOT_TRADE])
    for _ in range(3):
        write_simple(b, "spot", "trade", SPOT_TRADE, "spot_trade_frames.jsonl", g, limit=10)
        b.clock.advance(3600)
    b.stop()
    manifest = Manifest(tmp_path)
    names = sorted(manifest.live_segments())
    events: list[tuple[str, dict]] = []
    clock = FakeClock(wall_us=b.clock.wall_us() + 60 * US_PER_HOUR)
    plan_ = run_cleanup(
        tmp_path,
        manifest,
        clock,
        lambda k, **f: events.append((k, f)),
        warn_bytes=6 * GB,
        floor_bytes=2 * GB,
        retain_hours=48,
        free_bytes=int(5.99 * GB),
    )
    assert plan_.actions and plan_.actions[0].reason == "evicted_unacked"
    ev = [f for k, f in events if k == "EVICTED_UNACKED"][0]
    assert ev["name"] == names[0] and ev["sha256"] and ev["first_t"] and ev["last_t"] and ev["level_h"] == 48
    assert names[0] not in manifest.live_segments()


def test_ledger_shows_the_ladder_in_the_order_it_was_climbed(tmp_path):
    """acked and expired first, then the step-down announced right before the eviction it enables."""
    b = LakeBuilder(tmp_path)
    g = b.open_conn("spot_trade", [SPOT_TRADE])
    for _ in range(3):  # segments 30 hours apart: ages 70 h, 40 h and 10 h at cleanup time
        write_simple(b, "spot", "trade", SPOT_TRADE, "spot_trade_frames.jsonl", g, limit=10)
        b.clock.advance(30 * 3600)
    t_last = b.clock.wall_us() - 30 * US_PER_HOUR
    b.stop()
    manifest = Manifest(tmp_path)
    live = manifest.live_segments()
    old, mid, young = sorted(live)
    ack = ack_dir(tmp_path)
    ack.mkdir()
    (ack / live[old]["sha256"]).write_bytes(b"")  # only the oldest was pulled
    free = 1_000_000
    warn = free + live[old]["bytes"] + live[mid]["bytes"] // 2  # one removal is not enough, two are
    events: list[tuple[str, dict]] = []
    run_cleanup(
        tmp_path,
        manifest,
        FakeClock(wall_us=t_last + 10 * US_PER_HOUR),
        lambda k, **f: events.append((k, f)),
        warn_bytes=warn,
        floor_bytes=1,
        retain_hours=48,
        free_bytes=free,
    )
    assert [k for k, _ in events] == ["PRUNED_ACKED", "RETENTION_STEPDOWN", "EVICTED_UNACKED"]
    assert events[1][1]["retain_hours"] == 24.0
    assert events[2][1]["name"] == mid and events[2][1]["level_h"] == 24.0
    assert set(manifest.live_segments()) == {young}


def test_recovered_originals_are_dropped_after_72_hours(tmp_path):
    rec = tmp_path / "recovered"
    rec.mkdir()
    old, new = rec / "old.part", rec / "new.part"
    old.write_bytes(b"x")
    new.write_bytes(b"x")
    import os

    now = FakeClock()
    os.utime(old, (0, 0))
    os.utime(new, (now.wall_us() / 1e6, now.wall_us() / 1e6))
    run_cleanup(
        tmp_path,
        Manifest(tmp_path),
        now,
        lambda *a, **k: None,
        warn_bytes=1,
        floor_bytes=0,
        retain_hours=48,
        free_bytes=GB,
    )
    assert not old.exists() and new.exists()


def test_reserve_file_lifecycle(tmp_path):
    assert ensure_reserve(tmp_path, 4096, free_bytes=10 * GB, warn_bytes=GB)
    assert reserve_path(tmp_path).stat().st_size == 4096
    assert release_reserve(tmp_path) == 4096 and not reserve_path(tmp_path).exists()
    assert release_reserve(tmp_path) == 0
    assert not ensure_reserve(tmp_path, 4096, free_bytes=GB, warn_bytes=GB)  # not enough headroom to reserve
    assert not reserve_path(tmp_path).exists()


@pytest.mark.parametrize("retain", [6.0, 12.0])
def test_plan_with_short_retention_periods(retain):
    segs = [seg("a7", 7, acked=True), seg("a3", 3, acked=True)]
    p = plan(segs, free_gb=10, retain=retain)
    assert [a.name for a in p.actions] == (["a7"] if retain == 6.0 else [])
