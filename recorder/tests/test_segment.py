from __future__ import annotations

from pathlib import Path

import pytest
import zstandard as zstd
from conftest import load_frames

from hy_recorder.clock import FakeClock
from hy_recorder.envelope import decode_record, encode_record
from hy_recorder.fsutil import sha256_file
from hy_recorder.ledger import Ledger, make_event
from hy_recorder.manifest import Manifest
from hy_recorder.segment import SegmentWriter, recover
from hy_recorder.state import StateFile

T0 = 1_790_691_993_000_000  # 2026-09-29T14:26:33Z in epoch microseconds


def make_writer(root: Path, clock: FakeClock, state: StateFile, **kw):
    manifest = Manifest(root)
    w = SegmentWriter(
        root, "spot", "depth", level=3, next_segseq=state.next_segseq, manifest=manifest, clock=clock, **kw
    )
    return w, manifest


def line(
    seq: int, wall_us: int, payload: bytes = b'{"a":1}', stream: str = "spot:btcusdt@depth@100ms"
) -> bytes:
    out, reason = encode_record(
        run=1, seq=seq, gen=1, wall_us=wall_us, mono_us=seq, stream=stream, payload=payload
    )
    assert reason is None
    return out


def read_all_lines(path: Path) -> list[bytes]:
    """Read a sealed segment the way generic tools do: a single-frame streaming decode."""
    with open(path, "rb") as fh:
        text = zstd.ZstdDecompressor().stream_reader(fh).read()
    return text.splitlines()


@pytest.fixture
def env(tmp_path):
    state = StateFile(tmp_path)
    state.load()
    state.begin_run()
    return tmp_path, FakeClock(wall_us=T0), state


def test_seal_produces_single_frame_readable_file_and_manifest(env):
    root, clock, state = env
    w, manifest = make_writer(root, clock, state)
    for i in range(1, 501):
        w.write(
            line(i, T0 + i * 1000), run=1, seq=i, wall_us=T0 + i * 1000, stream="spot:btcusdt@depth@100ms"
        )
        if i % 100 == 0:
            w.flush_block()
    ev = w.seal("test")
    assert ev is not None and ev["records"] == 500
    path = root / ev["name"]
    assert path.exists() and not Path(str(path) + ".part").exists()
    assert ev["sha256"] == sha256_file(path) and ev["bytes"] == path.stat().st_size
    assert ev["first"] == {"r": 1, "q": 1, "t": T0 + 1000} and ev["last"]["q"] == 500
    assert ev["streams"] == {"spot:btcusdt@depth@100ms": 500}
    lines = read_all_lines(path)  # default (first-frame-only) reader must see EVERY record
    assert len(lines) == 500
    assert [decode_record(x + b"\n").seq for x in lines] == list(range(1, 501))
    # zstd frame checksum is present and verified on full decode
    assert zstd.get_frame_parameters(path.read_bytes()).has_checksum
    assert manifest.live_segments()[ev["name"]]["sha256"] == ev["sha256"]


def test_a_quiet_class_still_seals_when_its_hour_is_over(env):
    """Hourly snapshots and daily reference data must not sit in an unsealed .part until the next record."""
    root, clock, state = env
    w, manifest = make_writer(root, clock, state)
    hour_end = ((T0 // 3_600_000_000) + 1) * 3_600_000_000
    w.write(line(1, T0), run=1, seq=1, wall_us=T0, stream="s")
    assert w.roll_if_due(T0 + 60_000_000) is None and w.is_open, "the hour is not over yet"
    assert w.roll_if_due(hour_end + 1_000_000) is None and w.is_open, (
        "grace period for records still in flight"
    )
    ev = w.roll_if_due(hour_end + 6_000_000)
    assert ev is not None and ev["reason"] == "rotate" and not w.is_open
    assert ev["name"] in manifest.live_segments() and (root / ev["name"]).exists()
    assert w.roll_if_due(hour_end + 7_000_000) is None, "nothing is open any more"
    # the next record of that class simply opens the next segment
    assert (
        w.write(line(2, hour_end + 8_000_000), run=1, seq=2, wall_us=hour_end + 8_000_000, stream="s") is None
    )
    assert w.is_open


def test_hour_rotation_and_unique_names(env):
    root, clock, state = env
    w, manifest = make_writer(root, clock, state)
    t_hour2 = ((T0 // 3_600_000_000) + 1) * 3_600_000_000 + 5
    w.write(line(1, T0), run=1, seq=1, wall_us=T0, stream="s")
    sealed = w.write(line(2, t_hour2), run=1, seq=2, wall_us=t_hour2, stream="s")
    assert sealed is not None and sealed["reason"] == "rotate"
    # a wall-clock step backwards must not reopen an old name or rotate again
    assert w.write(line(3, T0), run=1, seq=3, wall_us=T0, stream="s") is None
    w.seal("end")
    names = sorted(manifest.live_segments())
    assert len(names) == 2 and len(set(names)) == 2
    assert names[0].startswith("raw/spot/depth/20260929/14-") and names[1].startswith(
        "raw/spot/depth/20260929/15-"
    )


def test_size_cap_rotates(env):
    root, clock, state = env
    w, manifest = make_writer(root, clock, state, max_bytes=2000)
    import random

    rng = random.Random(1)
    for i in range(1, 200):
        payload = ('{"x":"%s"}' % "".join(rng.choice("abcdef0123456789") for _ in range(200))).encode()
        w.write(line(i, T0 + i, payload), run=1, seq=i, wall_us=T0 + i, stream="s")
        w.flush_block()
    w.seal("end")
    assert len(manifest.live_segments()) >= 2


def test_exclusive_create_never_overwrites(env):
    root, clock, state = env
    manifest = Manifest(root)
    w = SegmentWriter(root, "spot", "depth", level=3, next_segseq=lambda: 7, manifest=manifest, clock=clock)
    w.write(line(1, T0), run=1, seq=1, wall_us=T0, stream="s")
    w.abort_for_test()
    w2 = SegmentWriter(root, "spot", "depth", level=3, next_segseq=lambda: 7, manifest=manifest, clock=clock)
    with pytest.raises(FileExistsError):
        w2.write(line(1, T0), run=1, seq=1, wall_us=T0, stream="s")


def test_crash_leaves_part_and_recovery_reencodes_single_frame(env):
    root, clock, state = env
    w, manifest = make_writer(root, clock, state)
    for i in range(1, 301):
        w.write(line(i, T0 + i * 10), run=1, seq=i, wall_us=T0 + i * 10, stream="s")
        if i % 50 == 0:
            w.flush_block()
    w.flush_block()
    w.sync()
    part = root / (w.rel_name + ".part")
    w.abort_for_test()  # crash: no finish, no checksum
    assert part.exists()
    # simulate a torn tail: cut the file in the middle of a block
    data = part.read_bytes()
    part.write_bytes(data[: len(data) - 7])

    events: list[tuple[str, dict]] = []
    state2 = StateFile(root)
    state2.load()
    created = recover(
        root,
        manifest=Manifest(root),
        level=3,
        next_segseq=state2.next_segseq,
        clock=clock,
        emit=lambda kind, **f: events.append((kind, f)),
    )
    assert len(created) == 1
    ev = created[0]
    assert ev["recovered"] and ev["truncated"] and ev["records"] >= 200
    lines = read_all_lines(root / ev["name"])
    assert len(lines) == ev["records"]
    assert [decode_record(x + b"\n").seq for x in lines] == list(range(1, len(lines) + 1))  # a clean prefix
    assert zstd.get_frame_parameters((root / ev["name"]).read_bytes()).has_checksum
    assert not part.exists() and (root / "recovered" / part.name).exists()  # original kept
    assert events and events[0][0] == "SEGMENT_RECOVERED"
    # recovery is idempotent
    again = recover(
        root,
        manifest=Manifest(root),
        level=3,
        next_segseq=state2.next_segseq,
        clock=clock,
        emit=lambda *a, **k: None,
    )
    assert again == []


def test_sealed_but_unregistered_segment_is_registered_late(env):
    root, clock, state = env
    w, manifest = make_writer(root, clock, state)
    w.write(line(1, T0), run=1, seq=1, wall_us=T0, stream="s")
    ev = w.seal("x")
    # lose the manifest (crash between rename and manifest append)
    for fp in (root / "manifest").glob("*.jsonl"):
        fp.unlink()
    events: list = []
    created = recover(
        root,
        manifest=Manifest(root),
        level=3,
        next_segseq=state.next_segseq,
        clock=clock,
        emit=lambda kind, **f: events.append(kind),
    )
    assert (
        len(created) == 1 and created[0]["name"] == ev["name"] and created[0]["reason"] == "registered_late"
    )
    assert created[0]["sha256"] == ev["sha256"]
    assert Manifest(root).live_segments()[ev["name"]]["recovered"] is True


def test_manifest_entry_without_file_is_reported_dangling(env):
    root, clock, state = env
    w, manifest = make_writer(root, clock, state)
    w.write(line(1, T0), run=1, seq=1, wall_us=T0, stream="s")
    ev = w.seal("x")
    (root / ev["name"]).unlink()
    events: list = []
    recover(
        root,
        manifest=Manifest(root),
        level=3,
        next_segseq=state.next_segseq,
        clock=clock,
        emit=lambda kind, **f: events.append((kind, f)),
    )
    assert ("MANIFEST_DANGLING", {"name": ev["name"]}) in events


def test_state_run_no_never_decreases_and_is_recovered_from_disk(tmp_path):
    s = StateFile(tmp_path)
    s.load()
    assert s.begin_run() == (1, True)
    s.mark_clean()
    assert s.begin_run() == (2, True)  # previous shutdown was clean
    assert s.begin_run() == (3, False)  # run 2 never marked clean
    assert s.next_segseq() == 1 and s.next_segseq() == 2
    # ledger evidence of run 9 and a segment numbered 41, then the state file is lost
    led = Ledger(tmp_path)
    led.append(make_event(9, 1, T0, 1, "PROC_START"), force_sync=True)
    led.close()
    seg = tmp_path / "raw" / "spot" / "depth" / "20260929" / "14-000041.jsonl.zst"
    seg.parent.mkdir(parents=True)
    seg.write_bytes(b"x")
    (tmp_path / "state.json").unlink()
    s2 = StateFile(tmp_path)
    s2.load()
    assert s2.recovered
    run_no, prev_clean = s2.begin_run()
    assert run_no == 10 and prev_clean is False
    assert s2.next_segseq() > 41


def test_ledger_orders_by_run_and_seq_and_survives_partial_line(tmp_path):
    led = Ledger(tmp_path)
    led.append(make_event(2, 5, T0, 1, "B"))
    led.append(make_event(1, 9, T0, 1, "A"))
    led.append(make_event(2, 1, T0, 1, "C", x=1))
    led.close()
    with next((tmp_path / "ledger").glob("*.jsonl")).open("ab") as fh:
        fh.write(b'{"run":2,"q":')  # torn write at the tail
    events = Ledger.read_all(tmp_path)
    assert any(e["k"] == "CORRUPT_LINE" for e in events)
    good = [e for e in events if e["k"] in ("A", "B", "C")]
    assert [e["k"] for e in good] == ["A", "C", "B"]  # (run, q) order: (1,9) (2,1) (2,5)


def test_real_frames_pass_through_a_segment_byte_exact(env):
    root, clock, state = env
    w, manifest = make_writer(root, clock, state)
    frames = load_frames("spot_depth_bridge_frames.jsonl")
    for i, (_, _, payload) in enumerate(frames, 1):
        out, reason = encode_record(
            run=1, seq=i, gen=1, wall_us=T0 + i, mono_us=i, stream="spot:btcusdt@depth@100ms", payload=payload
        )
        assert reason is None
        w.write(out, run=1, seq=i, wall_us=T0 + i, stream="spot:btcusdt@depth@100ms")
    ev = w.seal("x")
    back = [decode_record(x + b"\n").payload for x in read_all_lines(root / ev["name"])]
    assert back == [p for _, _, p in frames]
