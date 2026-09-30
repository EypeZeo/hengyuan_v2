from __future__ import annotations

import time

from hy_recorder.monitor import EXIT_LOOP_STALL, EXIT_WRITER_STALL, Monitor


class Box:
    def __init__(self):
        self.now = 100.0
        self.loop = 100.0
        self.writer = 100.0
        self.exits: list[int] = []
        self.logs: list[str] = []

    def monitor(self, **kw):
        return Monitor(
            lambda: self.loop,
            lambda: self.writer,
            exit_fn=self.exits.append,
            log=self.logs.append,
            now=lambda: self.now,
            **kw,
        )


def test_healthy_loop_and_writer_never_fire_even_when_no_data_arrives():
    b = Box()
    m = b.monitor()
    for _ in range(100):
        b.now += 1.0
        b.loop = b.now  # heartbeats keep advancing: a silent market is not a stall
        b.writer = b.now
        assert m.check_once() is None
    assert b.exits == []


def test_loop_stall_exits_with_its_own_code():
    b = Box()
    m = b.monitor(loop_limit_s=30)
    b.now = 129.0
    b.writer = b.now
    assert m.check_once() is None
    b.now = 131.0
    b.writer = b.now
    assert m.check_once() == EXIT_LOOP_STALL and b.exits == [EXIT_LOOP_STALL]
    assert "event loop stalled" in b.logs[0]


def test_writer_stall_exits_with_its_own_code():
    b = Box()
    m = b.monitor(writer_limit_s=60)
    b.now = 159.0
    b.loop = b.now
    assert m.check_once() is None
    b.now = 161.0
    b.loop = b.now
    assert m.check_once() == EXIT_WRITER_STALL and b.exits == [EXIT_WRITER_STALL]


def test_thread_fires_once_and_stops():
    b = Box()
    b.now = 1000.0  # far beyond both limits from the start
    m = b.monitor(interval_s=0.01)
    m.start()
    m.join(2.0)
    assert not m.is_alive() and b.exits == [EXIT_LOOP_STALL]


def test_thread_can_be_stopped_without_firing():
    b = Box()
    m = b.monitor(interval_s=0.01)
    m.start()
    time.sleep(0.05)
    m.stop()
    m.join(2.0)
    assert not m.is_alive() and b.exits == [] and m.fired is None
