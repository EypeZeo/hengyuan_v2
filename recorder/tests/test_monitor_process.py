"""Real-process checks of the stall monitor: it kills a stuck process and leaves a healthy one alone."""

from __future__ import annotations

import asyncio
import os
import signal
import subprocess
import sys
import textwrap
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]


def run_child(code: str, timeout: float = 60.0) -> subprocess.CompletedProcess[str]:
    env = {**os.environ, "PYTHONPATH": str(ROOT)}
    return subprocess.run(
        [sys.executable, "-c", textwrap.dedent(code)],
        capture_output=True,
        text=True,
        timeout=timeout,
        cwd=ROOT,
        env=env,
    )


def test_a_blocked_event_loop_makes_the_process_exit_with_code_70():
    child = """
        import asyncio, time
        from hy_recorder.monitor import Monitor

        beat = [time.monotonic()]
        Monitor(lambda: beat[0], lambda: time.monotonic(), loop_limit_s=0.5, writer_limit_s=60,
                interval_s=0.1).start()

        async def main():
            beat[0] = time.monotonic()
            time.sleep(20)  # a blocking call on the loop: the heartbeat cannot advance

        asyncio.run(main())
    """
    started = time.monotonic()
    r = run_child(child)
    assert r.returncode == 70, (r.returncode, r.stderr)
    assert "event loop stalled" in r.stderr
    assert time.monotonic() - started < 15, "the monitor must not wait for the blocking call to end"


def test_a_stuck_writer_thread_makes_the_process_exit_with_code_71(tmp_path):
    child = """
        import sys, time
        from pathlib import Path
        from hy_recorder.clock import SYSTEM_CLOCK
        from hy_recorder.config import default_config
        from hy_recorder.ledger import Ledger
        from hy_recorder.manifest import Manifest
        from hy_recorder.monitor import Monitor
        from hy_recorder.seq import SeqCounter
        from hy_recorder.state import StateFile
        from hy_recorder.writer import Writer

        root = Path(%r)
        cfg = default_config(root, reserve_mb=1)
        state = StateFile(root); state.load(); run_no, _ = state.begin_run()
        w = Writer(cfg, state=state, manifest=Manifest(root), ledger=Ledger(root), clock=SYSTEM_CLOCK,
                   seq=SeqCounter(), run_no=run_no)
        Writer._write_one = lambda self, key, item: time.sleep(30)  # writer hangs on its first record
        w.start()
        w.submit_record("spot", "trade", b"x\\n", seq=1, wall_us=1, stream="spot:btcusdt@trade")
        Monitor(lambda: time.monotonic(), lambda: w.hb_mono, writer_limit_s=0.6, interval_s=0.1).start()
        time.sleep(30)
    """ % str(tmp_path)
    r = run_child(child)
    assert r.returncode == 71, (r.returncode, r.stderr)
    assert "writer stalled" in r.stderr


def test_a_healthy_process_is_left_alone():
    child = """
        import asyncio, time
        from hy_recorder.monitor import Monitor

        beat = [time.monotonic()]
        Monitor(lambda: beat[0], lambda: time.monotonic(), loop_limit_s=0.5, writer_limit_s=0.5,
                interval_s=0.1).start()

        async def main():
            for _ in range(20):
                await asyncio.sleep(0.1)
                beat[0] = time.monotonic()

        asyncio.run(main())
        print("clean")
    """
    r = run_child(child)
    assert r.returncode == 0 and "clean" in r.stdout, (r.returncode, r.stderr)


@pytest.mark.skipif(sys.platform == "win32", reason="POSIX signals")
def test_run_forever_stops_gracefully_on_sigterm(monkeypatch, tmp_path):
    import hy_recorder.recorder as recorder_mod
    from hy_recorder.config import default_config

    seen = {}

    class FakeRecorder:
        def __init__(self, cfg):
            seen["cfg"] = cfg

        async def run(self, stop):
            asyncio.get_running_loop().call_later(0.2, os.kill, os.getpid(), signal.SIGTERM)
            await stop.wait()
            seen["stopped"] = True
            return 0

    monkeypatch.setattr(recorder_mod, "Recorder", FakeRecorder)
    assert recorder_mod.run_forever(default_config(tmp_path)) == 0
    assert seen["stopped"] is True
