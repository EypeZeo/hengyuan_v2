from __future__ import annotations

import errno
import json
import time

import pytest

from hy_recorder.clock import FakeClock
from hy_recorder.config import default_config
from hy_recorder.envelope import encode_record
from hy_recorder.ledger import Ledger, make_event
from hy_recorder.manifest import Manifest
from hy_recorder.segment import SegmentWriter
from hy_recorder.seq import SeqCounter
from hy_recorder.state import StateFile
from hy_recorder.verify import verify_lake
from hy_recorder.writer import GB, Writer

T0 = 1_790_691_993_000_000
STREAM = "spot:btcusdt@trade"


class Rig:
    def __init__(self, tmp_path, **cfg):
        cfg.setdefault("reserve_mb", 1)
        self.cfg = default_config(tmp_path, **cfg)
        self.clock = FakeClock(wall_us=T0)
        self.state = StateFile(tmp_path)
        self.state.load()
        self.run_no, _ = self.state.begin_run()
        self.seq = SeqCounter()
        self.manifest = Manifest(tmp_path)
        self.ledger = Ledger(tmp_path)
        self.writer = Writer(
            self.cfg,
            state=self.state,
            manifest=self.manifest,
            ledger=self.ledger,
            clock=self.clock,
            seq=self.seq,
            run_no=self.run_no,
        )

    def event(self, kind, **f):
        ev = make_event(self.run_no, self.seq.next(), self.clock.wall_us(), self.clock.mono_us(), kind, **f)
        self.writer.submit_event(ev)

    def rec(self, payload: bytes | None = None, *, tid: int = 0, gen: int = 1) -> bool:
        seq = self.seq.next()
        payload = (
            payload or json.dumps({"stream": "btcusdt@trade", "data": {"e": "trade", "t": tid}}).encode()
        )
        line, _ = encode_record(
            run=self.run_no,
            seq=seq,
            gen=gen,
            wall_us=self.clock.wall_us(),
            mono_us=self.clock.mono_us(),
            stream=STREAM,
            payload=payload,
        )
        return self.writer.submit_record(
            "spot", "trade", line, seq=seq, wall_us=self.clock.wall_us(), stream=STREAM
        )

    def finish(self):
        self.writer.stop_and_join(30)
        assert not self.writer.is_alive()


def test_clean_run_writes_everything_seals_and_verifies(tmp_path):
    rig = Rig(tmp_path)
    rig.event("PROC_START")
    rig.event("WS_OPEN", conn="spot_trade", gen=1, streams=[STREAM])
    rig.writer.start()
    for i in range(1, 2001):
        assert rig.rec(tid=i)
        rig.clock.advance(0.001)
    rig.finish()
    rep = verify_lake(tmp_path)
    assert rep.exit_code == 0, rep.render_text()
    assert rep.streams[STREAM]["records"] == 2000
    assert rep.runs[1]["clean"] is True
    st = StateFile(tmp_path)
    st.load()
    assert st.get("clean") is True
    status = json.loads((tmp_path / "status.json").read_bytes())
    assert (
        status["queues"]["spot/trade"]["written"] == 2000 and status["queues"]["spot/trade"]["dropped"] == 0
    )


def test_producer_never_blocks_and_every_drop_is_accounted_by_overrun_events(tmp_path):
    rig = Rig(tmp_path, queue_capacity=100)
    rig.event("WS_OPEN", conn="spot_trade", gen=1, streams=[STREAM])
    t0 = time.perf_counter()
    accepted = [
        rig.rec(tid=i) for i in range(1, 1001)
    ]  # the writer thread is not running yet: queue fills at 100
    elapsed = time.perf_counter() - t0
    assert sum(accepted) == 100 and elapsed < 1.0, "submit must be non-blocking"
    rig.writer.start()
    rig.finish()
    events = Ledger.read_all(tmp_path)
    over = [e for e in events if e["k"] == "OVERRUN"]
    assert over and sum(e["dropped"] for e in over) == 900
    covered = set()
    for e in over:
        covered.update(range(e["first_q"], e["last_q"] + 1))
    dropped_q = set(range(2 + 100, 2 + 1000))  # q=1 is the WS_OPEN event, records are q=2..1001
    assert dropped_q <= covered
    rep = verify_lake(tmp_path)
    # the sequence holes are explained by the ledger; the trade-id gap in the surviving data is too
    assert rep.exit_code == 0, rep.render_text()
    assert "SEQ_HOLE" not in {i.code for i in rep.issues}


def test_graceful_stop_drains_the_queue_without_loss(tmp_path):
    rig = Rig(tmp_path)
    rig.event("WS_OPEN", conn="spot_trade", gen=1, streams=[STREAM])
    for i in range(1, 501):
        assert rig.rec(tid=i)
    rig.writer.start()
    rig.writer.request_stop()  # stop immediately, records still queued
    rig.writer.join(30)
    rep = verify_lake(tmp_path)
    assert rep.exit_code == 0 and rep.streams[STREAM]["records"] == 500


def test_records_after_stop_request_are_refused_and_accounted(tmp_path):
    rig = Rig(tmp_path)
    rig.event("WS_OPEN", conn="spot_trade", gen=1, streams=[STREAM])
    rig.writer.start()
    rig.rec(tid=1)
    rig.writer.request_stop()
    assert rig.rec(tid=2) is False
    rig.writer.join(30)
    over = [e for e in Ledger.read_all(tmp_path) if e["k"] == "OVERRUN"]
    assert sum(e["dropped"] for e in over) == 1


def test_enospc_during_a_write_releases_the_reserve_stops_writing_and_still_shuts_down_cleanly(
    tmp_path, monkeypatch
):
    rig = Rig(tmp_path)
    rig.event("WS_OPEN", conn="spot_trade", gen=1, streams=[STREAM])
    from hy_recorder.retention import ensure_reserve, reserve_path

    assert ensure_reserve(tmp_path, 4096, free_bytes=100 * GB, warn_bytes=GB)
    real_write = SegmentWriter.write
    calls = {"n": 0}

    def flaky(self, line, **kw):
        calls["n"] += 1
        if calls["n"] > 20:
            raise OSError(errno.ENOSPC, "No space left on device")
        return real_write(self, line, **kw)

    monkeypatch.setattr(SegmentWriter, "write", flaky)
    for i in range(1, 101):
        rig.rec(tid=i)
    rig.writer.start()
    rig.finish()
    events = Ledger.read_all(tmp_path)
    kinds = [e["k"] for e in events]
    assert "WRITE_ERROR" in kinds and "DISK_STOP" in kinds and "RESERVE_RELEASED" in kinds
    assert not reserve_path(tmp_path).exists()
    assert kinds[-1] == "PROC_STOP"  # still shut down gracefully
    assert rig.writer.disk_stopped


def test_low_disk_triggers_cleanup_then_stop_then_resume(tmp_path, monkeypatch):
    rig = Rig(tmp_path, disk_warn_gb=5.0, disk_floor_gb=2.0)
    free = {"v": 1 * GB}
    monkeypatch.setattr(Writer, "_free", lambda self: free["v"])
    w = rig.writer
    w._disk_check()  # below warn: warning + cleanup attempt; below floor: stop
    assert w.disk_stopped
    kinds = [e["k"] for e in _events(w)]
    assert "DISK_WARN" in kinds and "DISK_STOP" in kinds
    assert (
        w.submit_record("spot", "trade", b"x\n", seq=1, wall_us=T0, stream=STREAM) is False
    )  # refused while stopped
    free["v"] = 6 * GB
    w._disk_check()
    assert not w.disk_stopped and "DISK_RESUMED" in [e["k"] for e in _events(w)]


def _events(w: Writer) -> list[dict]:
    """Flush what the (unstarted) writer emitted synchronously into the ledger and read it back."""
    w._drain_control()
    w.ledger.sync()
    return Ledger.read_all(w.cfg.root)


def test_status_file_reports_queues_disk_and_runway(tmp_path):
    rig = Rig(tmp_path)
    rig.event("WS_OPEN", conn="spot_trade", gen=1, streams=[STREAM])
    rig.rec(tid=1)
    rig.writer._write_status()
    st = json.loads((tmp_path / "status.json").read_bytes())
    assert st["run"] == 1 and st["queues"]["spot/trade"]["depth"] == 1
    assert st["disk"]["free_bytes"] > 0 and st["disk"]["stopped"] is False and "runway_hours" in st["disk"]


def test_overrun_ranges_are_coalesced_and_bounded(tmp_path):
    rig = Rig(tmp_path, queue_capacity=100)
    w = rig.writer
    for q in range(1, 1000, 10):  # 100 isolated drops, 10 apart: no merging possible
        w._note_drop(("spot", "trade"), q)
    w._emit_overruns()
    over = [e for e in _events(w) if e["k"] == "OVERRUN"]
    assert (
        len(over) == 50 and sum(e["dropped"] for e in over) == 100
    )  # capped, tail merged into one wide range
    w._note_drop(("spot", "trade"), 5000)
    w._note_drop(("spot", "trade"), 5002)  # within the merge distance
    w._emit_overruns()
    last = [e for e in _events(w) if e["k"] == "OVERRUN"][-1]
    assert (last["first_q"], last["last_q"], last["dropped"]) == (5000, 5002, 2)


@pytest.mark.parametrize("n", [3])
def test_heartbeat_advances_while_idle(tmp_path, n):
    rig = Rig(tmp_path)
    rig.writer.start()
    a = rig.writer.hb_mono
    time.sleep(0.2)
    assert rig.writer.hb_mono > a
    rig.finish()
