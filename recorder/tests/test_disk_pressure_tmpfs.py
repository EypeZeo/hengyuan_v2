"""Disk-pressure acceptance on a REAL small filesystem (D0-2/D0-3): skipped unless HY_TEST_TMPFS is set.

    mount -t tmpfs -o size=64m tmpfs /mnt/hy-tmpfs
    HY_TEST_TMPFS=/mnt/hy-tmpfs python3 -m pytest tests/test_disk_pressure_tmpfs.py -q

Unit tests already prove the retention ladder with injected free-space numbers; these two prove the same
behaviour when the numbers come from ``statvfs`` and the failure is a genuine ENOSPC from the kernel.
"""

from __future__ import annotations

import os
import shutil
import time
from pathlib import Path

import pytest
from lake import SPOT_TRADE, LakeBuilder

from hy_recorder.clock import SYSTEM_CLOCK, FakeClock
from hy_recorder.config import default_config
from hy_recorder.envelope import encode_record
from hy_recorder.ledger import Ledger
from hy_recorder.manifest import Manifest
from hy_recorder.seq import SeqCounter
from hy_recorder.state import StateFile
from hy_recorder.verify import verify_lake
from hy_recorder.writer import GB, Writer

TMPFS = os.environ.get("HY_TEST_TMPFS")
pytestmark = pytest.mark.skipif(not TMPFS, reason="set HY_TEST_TMPFS to a small tmpfs mount (about 64 MB)")

MB = 1 << 20


def fresh(name: str) -> Path:
    root = Path(TMPFS) / name
    shutil.rmtree(root, ignore_errors=True)
    root.mkdir(parents=True)
    return root


def free_bytes() -> int:
    return shutil.disk_usage(TMPFS).free


def junk_payload(nbytes: int) -> bytes:
    return os.urandom(nbytes // 2).hex().encode()  # incompressible-ish text of about nbytes bytes


def aged_lake(root: Path, ages_h: list[float], seg_mb: float) -> None:
    """One sealed segment per age (hours before now), each about ``seg_mb`` MB compressed."""
    now_us = time.time_ns() // 1000
    b = LakeBuilder(root, level=1)
    for age in ages_h:
        b.clock = FakeClock(wall_us=now_us - int(age * 3600 * 1e6))
        gen = b.open_conn("spot_trade", [SPOT_TRADE])
        for _ in range(int(seg_mb * 10)):
            b.record("spot", "trade", SPOT_TRADE, junk_payload(100_000), gen, dt_us=10)
        for w in b.writers.values():
            w.seal("test")
        b.writers.clear()
    b.stop()


def make_writer(root: Path, **cfg_kw) -> Writer:
    cfg = default_config(root, **cfg_kw)
    state = StateFile(root)
    state.load()
    run_no, _ = state.begin_run()
    return Writer(
        cfg,
        state=state,
        manifest=Manifest(root),
        ledger=Ledger(root),
        clock=SYSTEM_CLOCK,
        seq=SeqCounter(start=1_000_000),
        run_no=run_no,
    )


def test_the_retention_ladder_reclaims_space_in_order_and_never_stops_writing():
    root = fresh("ladder")
    aged_lake(root, [72, 40, 20, 8], seg_mb=6)
    live = Manifest(root).live_segments()
    assert len(live) == 4
    oldest = sorted(live.values(), key=lambda e: e["first"]["t"])[0]
    (root / "acks").mkdir()
    (root / "acks" / oldest["sha256"]).touch()  # only the 72 h old segment was pulled and acknowledged

    free = free_bytes()
    seg = sum(e["bytes"] for e in live.values()) // 4
    # warn just above what removing ONE segment can restore, so the ladder has to go down a step
    w = make_writer(
        root, disk_warn_gb=(free + seg * 1.6) / GB, disk_floor_gb=(free - 12 * MB) / GB, reserve_mb=1
    )
    w._disk_check()
    events = Ledger.read_all(root)
    kinds = [
        e["k"]
        for e in events
        if e["k"] in ("PRUNED_ACKED", "RETENTION_STEPDOWN", "EVICTED_UNACKED", "DISK_STOP")
    ]
    assert kinds == ["PRUNED_ACKED", "RETENTION_STEPDOWN", "EVICTED_UNACKED"], kinds
    assert not w.disk_stopped
    evicted = next(e for e in events if e["k"] == "EVICTED_UNACKED")
    assert evicted["level_h"] == 24 and evicted["name"] != oldest["name"]
    assert free_bytes() >= free + 2 * seg * 0.9
    survivors = set(Manifest(root).live_segments())
    assert len(survivors) == 2 and all((root / n).exists() for n in survivors)

    # and the recorder can still write afterwards
    w.start()
    line, _ = encode_record(
        run=w.run_no,
        seq=w.seq.next(),
        gen=1,
        wall_us=SYSTEM_CLOCK.wall_us(),
        mono_us=1,
        stream=SPOT_TRADE,
        payload=b'{"stream":"btcusdt@trade","data":{"t":1}}',
    )
    assert w.submit_record(
        "spot", "trade", line, seq=w.seq.last, wall_us=SYSTEM_CLOCK.wall_us(), stream=SPOT_TRADE
    )
    w.stop_and_join(30)
    assert not w.is_alive()
    report = verify_lake(root)
    assert "SEQ_HOLE" not in {i.code for i in report.issues}, report.render_text()


def test_a_genuine_enospc_releases_the_reserve_stops_writing_and_still_closes_cleanly():
    root = fresh("enospc")
    w = make_writer(root, disk_warn_gb=0.001, disk_floor_gb=0.0005, reserve_mb=2)
    w.start()
    time.sleep(0.5)  # the writer thread creates its reserve file first
    assert (root / "reserve.bin").exists()
    junk = Path(TMPFS) / "enospc-junk.bin"
    try:
        with open(junk, "wb") as fh:  # leave a few hundred KB free
            chunk = b"\0" * MB
            while free_bytes() > 512 * 1024:
                try:
                    fh.write(chunk)
                    fh.flush()
                except OSError:
                    break
        payload = junk_payload(200_000)
        for i in range(200):
            seq = w.seq.next()
            line, _ = encode_record(
                run=w.run_no,
                seq=seq,
                gen=1,
                wall_us=SYSTEM_CLOCK.wall_us(),
                mono_us=i,
                stream=SPOT_TRADE,
                payload=payload,
            )
            w.submit_record("spot", "trade", line, seq=seq, wall_us=SYSTEM_CLOCK.wall_us(), stream=SPOT_TRADE)
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline and not w.disk_stopped:
            time.sleep(0.1)
        assert w.disk_stopped, "a real ENOSPC must stop writing"
    finally:
        junk.unlink(missing_ok=True)
    w.stop_and_join(30)
    assert not w.is_alive()
    kinds = [e["k"] for e in Ledger.read_all(root)]
    assert "WRITE_ERROR" in kinds and "DISK_STOP" in kinds and "RESERVE_RELEASED" in kinds
    assert kinds[-1] == "PROC_STOP", "the reserve made room for a graceful shutdown record"
    assert not (root / "reserve.bin").exists()
