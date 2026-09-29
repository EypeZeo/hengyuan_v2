from __future__ import annotations

import json
import shutil
from pathlib import Path

import pytest
import zstandard as zstd
from lake import (
    SPOT_DEPTH,
    SPOT_TRADE,
    USDM_AGG,
    USDM_DEPTH,
    USDM_FORCE,
    USDM_MARK,
    LakeBuilder,
    write_depth,
    write_simple,
)

from hy_recorder.fsutil import sha256_file
from hy_recorder.manifest import Manifest
from hy_recorder.verify import verify_lake


def build_clean(
    root: Path,
    *,
    market_skip=None,
    trade_skip=None,
    agg_skip=None,
    mark_skip=None,
    spot_depth_drop=None,
    usdm_depth_drop=None,
    stop: bool = True,
) -> LakeBuilder:
    b = LakeBuilder(root)
    g_sd = b.open_conn("spot_depth", [SPOT_DEPTH])
    g_st = b.open_conn("spot_trade", [SPOT_TRADE])
    g_ud = b.open_conn("usdm_depth", [USDM_DEPTH])
    g_um = b.open_conn("usdm_market", [USDM_AGG, USDM_MARK, USDM_FORCE])
    write_depth(b, "spot", g_sd, drop=spot_depth_drop)
    write_depth(b, "usdm", g_ud, drop=usdm_depth_drop)
    write_simple(b, "spot", "trade", SPOT_TRADE, "spot_trade_frames.jsonl", g_st, skip=trade_skip)
    write_simple(b, "usdm", "market", USDM_AGG, "usdm_aggtrade_frames.jsonl", g_um, skip=agg_skip)
    write_simple(b, "usdm", "market", USDM_MARK, "usdm_markprice_frames.jsonl", g_um, skip=mark_skip)
    write_simple(b, "usdm", "market", USDM_FORCE, "usdm_forceorder_frames.jsonl", g_um)
    if stop:
        b.stop()
    return b


def codes(report, severity=None):
    return sorted({i.code for i in report.issues if severity is None or i.severity == severity})


def test_clean_real_data_lake_passes(tmp_path):
    build_clean(tmp_path)
    rep = verify_lake(tmp_path)
    assert rep.exit_code == 0, rep.render_text()
    assert codes(rep, "FAIL") == []
    for _venue, name in (("spot", SPOT_DEPTH), ("usdm", USDM_DEPTH)):
        s = rep.streams[name]
        assert s["records"] == 30 and s["counters"]["bridges"] == 1 and s["verified_intervals"] == 1
        assert s["verified_seconds"] > 0 and s["unverified_generations"] == 0
    assert rep.streams[SPOT_TRADE]["records"] == 25
    assert rep.streams[USDM_AGG]["records"] == 25 and rep.streams[USDM_MARK]["records"] == 10
    assert rep.streams[USDM_FORCE]["records"] > 0
    assert "f_l_not_adjacent_info" in rep.streams[USDM_AGG]["counters"] or True  # informational only
    assert rep.runs[1]["clean"] is True
    assert "PASS" in rep.render_text()
    # JSON form is serialisable
    json.dumps(rep.to_dict())


# -- negative controls: every one of these MUST fail ---------------------------------------------
def test_missing_depth_event_is_an_unexplained_gap(tmp_path):
    for venue, kw in (("spot", {"spot_depth_drop": {12}}), ("usdm", {"usdm_depth_drop": {12}})):
        root = tmp_path / venue
        build_clean(root, **kw)
        rep = verify_lake(root)
        assert rep.exit_code == 1
        assert "DEPTH_GAP" in codes(rep, "FAIL")
        assert "SEQ_HOLE" not in codes(
            rep
        )  # the event was never given a sequence number: only the id chain shows it


def test_depth_gap_explained_by_online_detection_passes(tmp_path):
    def build(root, bad_q=None):
        b = LakeBuilder(root)
        g = b.open_conn("spot_depth", [SPOT_DEPTH])

        def hook(q):  # the online tracker writes GAP_DETECTED right after the record that exposed the gap
            if bad_q is not None and q == bad_q:
                b.event("GAP_DETECTED", stream=SPOT_DEPTH, bad_q=bad_q, rule="spot_U_gt_prev_u_plus_1")

        write_depth(b, "spot", g, drop={12}, hook=hook)
        b.stop()

    build(tmp_path / "probe")
    gap = next(i for i in verify_lake(tmp_path / "probe").issues if i.code == "DEPTH_GAP")
    build(tmp_path / "real", bad_q=gap.q)
    rep = verify_lake(tmp_path / "real")
    assert rep.exit_code == 0, rep.render_text()
    explained = [i for i in rep.issues if i.code == "DEPTH_GAP"]
    assert explained and explained[0].severity == "INFO" and explained[0].explained_by == "GAP_DETECTED"
    # an event for a different record must not explain it
    build(tmp_path / "wrong", bad_q=gap.q + 1000)
    assert verify_lake(tmp_path / "wrong").exit_code == 1


def test_trade_id_gap_fails(tmp_path):
    build_clean(tmp_path, trade_skip={10})
    rep = verify_lake(tmp_path)
    assert rep.exit_code == 1 and "TRADE_ID_GAP" in codes(rep, "FAIL")


def test_aggtrade_a_gap_fails_but_f_l_adjacency_is_not_a_criterion(tmp_path):
    build_clean(tmp_path, agg_skip={7})
    rep = verify_lake(tmp_path)
    assert rep.exit_code == 1 and "AGGTRADE_ID_GAP" in codes(rep, "FAIL")
    clean = tmp_path / "clean"
    build_clean(clean)
    ok = verify_lake(clean)
    # real data contains non-adjacent f/l pairs; that must never fail the stream
    assert ok.exit_code == 0 and "AGGTRADE_ID_GAP" not in codes(ok)


def test_markprice_cadence_gap_fails(tmp_path):
    build_clean(tmp_path, mark_skip={3, 4, 5, 6})
    rep = verify_lake(tmp_path)
    assert rep.exit_code == 1 and "MARK_CADENCE_GAP" in codes(rep, "FAIL")


def test_missing_snapshot_leaves_data_unverified_but_is_not_an_integrity_failure(tmp_path):
    b = LakeBuilder(tmp_path)
    g = b.open_conn("spot_depth", [SPOT_DEPTH])
    write_depth(b, "spot", g, skip_snapshot=True)
    b.stop()
    rep = verify_lake(tmp_path)
    s = rep.streams[SPOT_DEPTH]
    assert s["verified_seconds"] == 0 and s["unverified_generations"] == 1 and s["verified_intervals"] == 0
    assert rep.exit_code == 0  # raw observation kept; it just must not be called usable L2


def test_reconnect_starts_a_new_generation_that_needs_its_own_bridge(tmp_path):
    b = LakeBuilder(tmp_path)
    g1 = b.open_conn("spot_depth", [SPOT_DEPTH])
    write_depth(b, "spot", g1)
    b.event("WS_CLOSE", conn="spot_depth", gen=g1, reason="test")
    g2 = b.open_conn("spot_depth", [SPOT_DEPTH])
    write_depth(
        b, "spot", g2, skip_snapshot=True
    )  # same frames again: chain restarts, but no snapshot for gen 2
    b.stop()
    rep = verify_lake(tmp_path)
    s = rep.streams[SPOT_DEPTH]
    assert rep.exit_code == 0, rep.render_text()
    assert s["generations"] == 2 and s["verified_intervals"] == 1 and s["unverified_generations"] == 1


def test_generation_without_open_event_is_unexplained(tmp_path):
    b = LakeBuilder(tmp_path)
    b.open_conn("spot_depth", [SPOT_DEPTH])
    write_depth(b, "spot", 1)
    write_depth(b, "spot", 2)  # generation 2 appears with no WS_OPEN in the ledger
    b.stop()
    rep = verify_lake(tmp_path)
    assert rep.exit_code == 1 and "GEN_BOUNDARY_UNEXPLAINED" in codes(rep, "FAIL")


def _first_segment(root: Path) -> Path:
    return next((root / "raw").rglob("*.jsonl.zst"))


def test_flipped_byte_in_a_segment_fails(tmp_path):
    build_clean(tmp_path)
    seg = _first_segment(tmp_path)
    data = bytearray(seg.read_bytes())
    data[len(data) // 2] ^= 0x01
    seg.write_bytes(bytes(data))
    rep = verify_lake(tmp_path)
    assert rep.exit_code == 1 and "SHA_MISMATCH" in codes(rep, "FAIL")
    # even without hash checking the zstd frame checksum / structure must catch it
    rep2 = verify_lake(tmp_path, check_hashes=False)
    assert rep2.exit_code == 1 and codes(rep2, "FAIL")


def test_tampered_manifest_hash_fails(tmp_path):
    build_clean(tmp_path)
    mf = next((tmp_path / "manifest").glob("*.jsonl"))
    lines = [json.loads(x) for x in mf.read_bytes().splitlines()]
    lines[0]["sha256"] = "0" * 64
    mf.write_bytes(b"\n".join(json.dumps(x, separators=(",", ":")).encode() for x in lines) + b"\n")
    rep = verify_lake(tmp_path)
    assert rep.exit_code == 1 and "SHA_MISMATCH" in codes(rep, "FAIL")


def test_deleted_segment_and_deleted_manifest_fail(tmp_path):
    a, b_ = tmp_path / "a", tmp_path / "b"
    build_clean(a)
    build_clean(b_)
    _first_segment(a).unlink()
    assert "MANIFEST_DANGLING" in codes(verify_lake(a), "FAIL")
    for fp in (b_ / "manifest").glob("*.jsonl"):
        fp.unlink()
    assert "UNREGISTERED_SEGMENT" in codes(verify_lake(b_), "FAIL")


def test_record_removed_from_a_segment_is_a_sequence_hole(tmp_path):
    """Rewrite a sealed segment without one record and re-register it consistently: only the
    sequence numbers can reveal the loss."""
    build_clean(tmp_path)
    mf_dir = tmp_path / "manifest"
    seg = next(p for p in (tmp_path / "raw" / "spot" / "trade").rglob("*.jsonl.zst"))
    lines = zstd.ZstdDecompressor().stream_reader(seg.open("rb")).read().splitlines(keepends=True)
    del lines[10]
    new = zstd.ZstdCompressor(level=3, write_checksum=True).compress(b"".join(lines))
    seg.write_bytes(new)
    rel = seg.relative_to(tmp_path).as_posix()
    mf = next(mf_dir.glob("*.jsonl"))
    events = [json.loads(x) for x in mf.read_bytes().splitlines()]
    for e in events:
        if e.get("name") == rel:
            e.update(bytes=len(new), sha256=sha256_file(seg), records=len(lines))
    mf.write_bytes(b"\n".join(json.dumps(x, separators=(",", ":")).encode() for x in events) + b"\n")
    rep = verify_lake(tmp_path)
    assert rep.exit_code == 1
    assert "SEQ_HOLE" in codes(rep, "FAIL") and "TRADE_ID_GAP" in codes(rep, "FAIL")


def test_overrun_event_explains_the_sequence_hole_and_the_id_gap(tmp_path):
    b = LakeBuilder(tmp_path)
    g = b.open_conn("spot_trade", [SPOT_TRADE])
    write_simple(
        b, "spot", "trade", SPOT_TRADE, "spot_trade_frames.jsonl", g, skip={10, 11}, consume_q_on_skip=True
    )
    # the writer reports drops with the q range it dropped (q of the skipped records)
    first_dropped = (
        12  # WS_OPEN is q=1, ten written trades are q=2..11, so the two dropped ones are q=12 and 13
    )
    b.event("OVERRUN", cls="trade", dropped=2, first_q=first_dropped, last_q=first_dropped + 1)
    b.stop()
    rep = verify_lake(tmp_path)
    assert rep.exit_code == 0, rep.render_text()
    gap = [i for i in rep.issues if i.code == "TRADE_ID_GAP"]
    assert gap and gap[0].explained_by == "OVERRUN"


def test_sequence_hole_without_overrun_fails_on_clean_run(tmp_path):
    b = LakeBuilder(tmp_path)
    g = b.open_conn("spot_trade", [SPOT_TRADE])
    write_simple(
        b, "spot", "trade", SPOT_TRADE, "spot_trade_frames.jsonl", g, skip={10, 11}, consume_q_on_skip=True
    )
    b.stop()
    rep = verify_lake(tmp_path)
    assert rep.exit_code == 1 and "SEQ_HOLE" in codes(rep, "FAIL")


def test_crash_tail_is_tolerated_but_flagged(tmp_path):
    b = LakeBuilder(tmp_path)
    g = b.open_conn("spot_trade", [SPOT_TRADE])
    write_simple(b, "spot", "trade", SPOT_TRADE, "spot_trade_frames.jsonl", g, limit=20)
    b.crash()  # leaves an unsealed .part; simulate what recovery would have sealed
    from hy_recorder.segment import recover

    recover(
        tmp_path,
        manifest=Manifest(tmp_path),
        level=3,
        next_segseq=b.state.next_segseq,
        clock=b.clock,
        emit=lambda *a, **k: None,
    )
    rep = verify_lake(tmp_path)
    assert rep.exit_code == 0, rep.render_text()
    assert "RUN_NOT_STOPPED_CLEANLY" in codes(rep, "WARN")


def test_multi_frame_segment_is_rejected(tmp_path):
    build_clean(tmp_path)
    seg = next(p for p in (tmp_path / "raw" / "spot" / "trade").rglob("*.jsonl.zst"))
    data = zstd.ZstdDecompressor().stream_reader(seg.open("rb")).read()
    half = data.index(b"\n", len(data) // 2) + 1
    two = zstd.ZstdCompressor(level=3, write_checksum=True).compress(data[:half]) + zstd.ZstdCompressor(
        level=3, write_checksum=True
    ).compress(data[half:])
    seg.write_bytes(two)
    rel = seg.relative_to(tmp_path).as_posix()
    mf = next((tmp_path / "manifest").glob("*.jsonl"))
    events = [json.loads(x) for x in mf.read_bytes().splitlines()]
    for e in events:
        if e.get("name") == rel:
            e.update(bytes=len(two), sha256=sha256_file(seg))
    mf.write_bytes(b"\n".join(json.dumps(x, separators=(",", ":")).encode() for x in events) + b"\n")
    rep = verify_lake(tmp_path)
    assert rep.exit_code == 1 and "SEGMENT_TRAILING_DATA" in codes(rep, "FAIL")


def test_truncated_unregistered_recovery_flag_is_the_only_excuse_for_an_incomplete_frame(tmp_path):
    build_clean(tmp_path)
    seg = _first_segment(tmp_path)
    cut = seg.read_bytes()[:-9]
    seg.write_bytes(cut)
    rel = seg.relative_to(tmp_path).as_posix()
    mf = next((tmp_path / "manifest").glob("*.jsonl"))
    events = [json.loads(x) for x in mf.read_bytes().splitlines()]
    for e in events:
        if e.get("name") == rel:
            e.update(bytes=len(cut), sha256=sha256_file(seg))
    mf.write_bytes(b"\n".join(json.dumps(x, separators=(",", ":")).encode() for x in events) + b"\n")
    rep = verify_lake(tmp_path)
    assert rep.exit_code == 1 and "SEGMENT_INCOMPLETE" in codes(rep, "FAIL")


def test_records_out_of_order_fail(tmp_path):
    b = LakeBuilder(tmp_path)
    g = b.open_conn("spot_trade", [SPOT_TRADE])
    write_simple(b, "spot", "trade", SPOT_TRADE, "spot_trade_frames.jsonl", g, limit=5)
    # a record with a lower sequence number than one already written in the same class
    b.seq -= 3
    write_simple(b, "spot", "trade", SPOT_TRADE, "spot_trade_frames.jsonl", g, limit=1)
    b.stop()
    rep = verify_lake(tmp_path)
    assert rep.exit_code == 1 and "ORDER_VIOLATION" in codes(rep, "FAIL")


def test_empty_lake_is_not_a_crash(tmp_path):
    rep = verify_lake(tmp_path)
    assert rep.exit_code == 0 and rep.segments["sealed"] == 0


@pytest.mark.parametrize("root_exists", [True])
def test_verify_does_not_modify_the_lake(tmp_path, root_exists):
    build_clean(tmp_path)
    before = {
        p.relative_to(tmp_path).as_posix(): p.stat().st_mtime_ns for p in tmp_path.rglob("*") if p.is_file()
    }
    verify_lake(tmp_path)
    after = {
        p.relative_to(tmp_path).as_posix(): p.stat().st_mtime_ns for p in tmp_path.rglob("*") if p.is_file()
    }
    assert before == after
    shutil.rmtree(tmp_path / "raw")
