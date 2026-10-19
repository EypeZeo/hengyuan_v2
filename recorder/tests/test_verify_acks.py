"""Operator acknowledgements of known verify failures (``hy_recorder.verify.acks``).

The property under test: an ack can downgrade exactly the gap it names and nothing else, and no mistake in
the file can make a FAIL disappear -- a bad line is its own FAIL, a stale ack is a WARN, a missing file
changes nothing.
"""

from __future__ import annotations

import json

import pytest
from lake import SPOT_TRADE, LakeBuilder, write_simple
from test_verify import build_clean, codes

from hy_recorder.cli import main
from hy_recorder.maint import MaintConfig, do_verify
from hy_recorder.verify import verify_lake
from hy_recorder.verify.acks import ACK_FILE, ACKABLE, MAX_ACK_BYTES, Ack, apply_acks, load_acks
from hy_recorder.verify.streams import Issue

GAP_CASES = [
    ("TRADE_ID_GAP", {"trade_skip": {10}}),
    ("AGGTRADE_ID_GAP", {"agg_skip": {7}}),
    ("MARK_CADENCE_GAP", {"mark_skip": {3, 4, 5, 6}}),
]


def gap_of(rep, code="AGGTRADE_ID_GAP", severity="FAIL"):
    return next(i for i in rep.issues if i.code == code and i.severity == severity)


def ack_dict(issue, **over):
    d = {
        "v": 1,
        "id": "ACK-1",
        "code": issue.code,
        "stream": issue.stream,
        "run": issue.run,
        "gen": issue.gen,
        "q": issue.q,
        "detail": dict(issue.detail),
        "reason": "exchange-side hole: connection healthy, no recorder event",
        "evidence": "markPrice kept flowing on the same connection; ledger quiet +-1 h",
        "by": "test",
        "at": "2026-10-19T08:00:00Z",
    }
    d.update(over)
    return d


def write_acks(root, *lines):
    path = root / ACK_FILE
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return path


def snapshot(root):
    return {p.relative_to(root).as_posix(): p.stat().st_mtime_ns for p in root.rglob("*") if p.is_file()}


def gap_lake(root, **kw):
    """A lake with one unexplained aggTrade gap, and that gap's FAIL issue."""
    build_clean(root, **(kw or {"agg_skip": {7}}))
    return gap_of(verify_lake(root))


# -- the happy path -------------------------------------------------------------------------------
@pytest.mark.parametrize(("code", "kw"), GAP_CASES)
def test_an_exact_ack_turns_the_gap_into_info_and_the_lake_passes(tmp_path, code, kw):
    build_clean(tmp_path, **kw)
    gap = gap_of(verify_lake(tmp_path), code)
    assert verify_lake(tmp_path).exit_code == 1
    write_acks(tmp_path, json.dumps(ack_dict(gap)))
    rep = verify_lake(tmp_path)
    assert rep.exit_code == 0, rep.render_text()
    assert [(i.severity, i.explained_by) for i in rep.issues if i.code == code] == [("INFO", "ACK:ACK-1")]
    assert rep.acks == {"file": ACK_FILE, "entries": 1, "applied": ["ACK-1"], "unmatched": [], "invalid": 0}
    lines = rep.render_text().splitlines()
    assert "acknowledged: 1 known gap(s): ACK-1" in lines
    assert lines[-1].startswith("RESULT: PASS (FAIL=0") and lines[-1].endswith(" ACKED=1)")
    assert json.loads(json.dumps(rep.to_dict()))["acks"]["applied"] == ["ACK-1"]


@pytest.mark.parametrize(("code", "kw"), GAP_CASES)
def test_the_ackable_detail_keys_are_what_the_verifier_attaches(tmp_path, code, kw):
    build_clean(tmp_path, **kw)
    gap = gap_of(verify_lake(tmp_path), code)
    assert set(gap.detail) == ACKABLE[code]
    assert all(isinstance(v, int) and not isinstance(v, bool) for v in gap.detail.values())


def test_without_a_file_nothing_changes(tmp_path):
    build_clean(tmp_path, agg_skip={7})
    rep = verify_lake(tmp_path)
    assert rep.exit_code == 1 and "AGGTRADE_ID_GAP" in codes(rep, "FAIL")
    assert rep.acks == {"file": ACK_FILE, "entries": 0, "applied": [], "unmatched": [], "invalid": 0}
    text = rep.render_text()
    assert "acknowledged" not in text and "ACKED" not in text


def test_a_clean_lake_keeps_its_result_line_format(tmp_path):
    build_clean(tmp_path)
    last = verify_lake(tmp_path).render_text().splitlines()[-1]
    assert last.startswith("RESULT: PASS (FAIL=0 WARN=") and last.endswith(")") and "ACKED" not in last


def test_verifying_with_an_ack_file_does_not_modify_the_lake(tmp_path):
    gap = gap_lake(tmp_path)
    path = write_acks(tmp_path, json.dumps(ack_dict(gap)))
    before, content = snapshot(tmp_path), path.read_bytes()
    assert verify_lake(tmp_path).exit_code == 0
    assert snapshot(tmp_path) == before and path.read_bytes() == content


# -- an ack matches one gap by its whole identity -----------------------------------------------------
def mutated(gap, name):
    if name in gap.detail:
        return {"detail": {**gap.detail, name: gap.detail[name] + 1}}
    if name == "stream":
        return {"stream": gap.stream.replace("btcusdt", "ethusdt")}
    if name == "code":
        return {"code": "TRADE_ID_GAP"}
    return {name: getattr(gap, name) + 1}


@pytest.mark.parametrize(
    "name", ["stream", "run", "gen", "q", "code", "prev", "got", "missing", "prev_q"]
)
def test_an_ack_that_differs_in_any_identity_field_matches_nothing(tmp_path, name):
    gap = gap_lake(tmp_path)
    over = mutated(gap, name)
    write_acks(tmp_path, json.dumps(ack_dict(gap, **over)))
    rep = verify_lake(tmp_path)
    assert rep.exit_code == 1 and "AGGTRADE_ID_GAP" in codes(rep, "FAIL")
    assert rep.acks["applied"] == [] and rep.acks["unmatched"] == ["ACK-1"] and rep.acks["invalid"] == 0
    stale = gap_of(rep, "ACK_UNMATCHED", "WARN")
    assert stale.detail == {"id": "ACK-1", "code": over.get("code", gap.code), "line": 1}
    assert "ACKED" not in rep.render_text()


def test_a_stale_ack_names_its_line_in_the_file(tmp_path):
    gap = gap_lake(tmp_path)
    stale = ack_dict(gap, id="STALE", q=gap.q + 1)
    write_acks(tmp_path, json.dumps(ack_dict(gap, id="GOOD")), "", json.dumps(stale))
    rep = verify_lake(tmp_path)
    assert rep.exit_code == 0 and rep.acks["applied"] == ["GOOD"] and rep.acks["unmatched"] == ["STALE"]
    assert gap_of(rep, "ACK_UNMATCHED", "WARN").detail == {"id": "STALE", "code": gap.code, "line": 3}


def test_one_ack_covers_one_gap_not_its_neighbour(tmp_path):
    build_clean(tmp_path, agg_skip={7, 15})
    gaps = [i for i in verify_lake(tmp_path).issues if i.code == "AGGTRADE_ID_GAP"]
    assert len(gaps) == 2 and gaps[0].q != gaps[1].q
    write_acks(tmp_path, json.dumps(ack_dict(gaps[0], id="A")))
    rep = verify_lake(tmp_path)
    assert rep.exit_code == 1
    assert sorted((i.q, i.severity) for i in rep.issues if i.code == "AGGTRADE_ID_GAP") == [
        (gaps[0].q, "INFO"),
        (gaps[1].q, "FAIL"),
    ]
    write_acks(tmp_path, json.dumps(ack_dict(gaps[0], id="A")), json.dumps(ack_dict(gaps[1], id="B")))
    rep = verify_lake(tmp_path)
    assert rep.exit_code == 0 and rep.acks["applied"] == ["A", "B"]
    assert rep.render_text().splitlines()[-1].endswith(" ACKED=2)")


def test_a_stale_ack_on_a_lake_without_that_gap_is_only_a_warning(tmp_path):
    gap = gap_lake(tmp_path / "gap")
    clean = tmp_path / "clean"
    build_clean(clean)
    write_acks(clean, json.dumps(ack_dict(gap)))
    rep = verify_lake(clean)
    assert rep.exit_code == 0 and codes(rep, "WARN").count("ACK_UNMATCHED") == 1
    assert rep.acks["unmatched"] == ["ACK-1"]


def test_an_ack_for_a_gap_the_ledger_already_explains_stays_unmatched(tmp_path):
    b = LakeBuilder(tmp_path)
    g = b.open_conn("spot_trade", [SPOT_TRADE])
    write_simple(
        b, "spot", "trade", SPOT_TRADE, "spot_trade_frames.jsonl", g, skip={10, 11}, consume_q_on_skip=True
    )
    b.event("OVERRUN", cls="trade", dropped=2, first_q=12, last_q=13)
    b.stop()
    explained = gap_of(verify_lake(tmp_path), "TRADE_ID_GAP", "INFO")
    assert explained.explained_by == "OVERRUN"
    write_acks(tmp_path, json.dumps(ack_dict(explained)))
    rep = verify_lake(tmp_path)
    assert rep.exit_code == 0
    assert [i.explained_by for i in rep.issues if i.code == "TRADE_ID_GAP"] == ["OVERRUN"]
    assert rep.acks["applied"] == [] and rep.acks["unmatched"] == ["ACK-1"]


@pytest.mark.parametrize("code", ["SEQ_HOLE", "SHA_MISMATCH", "DEPTH_GAP", "ORDER_VIOLATION", "NOPE"])
def test_integrity_sequence_and_depth_failures_cannot_be_acknowledged(tmp_path, code):
    b = LakeBuilder(tmp_path)
    g = b.open_conn("spot_trade", [SPOT_TRADE])
    write_simple(
        b, "spot", "trade", SPOT_TRADE, "spot_trade_frames.jsonl", g, skip={10, 11}, consume_q_on_skip=True
    )
    b.stop()
    hole = gap_of(verify_lake(tmp_path), "SEQ_HOLE")
    gap = gap_of(verify_lake(tmp_path), "TRADE_ID_GAP")
    write_acks(tmp_path, json.dumps(ack_dict(hole, code=code, stream="-", gen=0, detail=gap.detail)))
    rep = verify_lake(tmp_path)
    assert rep.exit_code == 1 and {"SEQ_HOLE", "TRADE_ID_GAP", "ACK_INVALID"} <= set(codes(rep, "FAIL"))
    assert rep.acks["entries"] == 0 and rep.acks["invalid"] == 1


# -- a mistake in the file can never make a FAIL disappear ------------------------------------------------
def _without(key):
    return lambda b: {k: v for k, v in b.items() if k != key}


def _with(**over):
    return lambda b: {**b, **over}


def _detail_without(key):
    return lambda b: {**b, "detail": {k: v for k, v in b["detail"].items() if k != key}}


def _detail_with(**over):
    return lambda b: {**b, "detail": {**b["detail"], **over}}


BAD_LINES = [
    ("not-json", lambda b: "this is not json", ""),
    ("array", lambda b: "[1, 2]", "not a JSON object"),
    ("number", lambda b: "42", "not a JSON object"),
    ("missing-reason", _without("reason"), "missing ['reason']"),
    ("unknown-field", _with(extra=1), "unknown ['extra']"),
    ("v-2", _with(v=2), "v must be 1"),
    ("v-true", _with(v=True), "v must be 1"),
    ("id-space", _with(id="has space"), "id must match"),
    ("id-empty", _with(id=""), "id must match"),
    ("id-number", _with(id=7), "id must match"),
    ("code-unknown", _with(code="NOPE"), "code must be one of"),
    ("code-list", _with(code=["AGGTRADE_ID_GAP"]), "code must be one of"),
    ("stream-empty", _with(stream=""), "stream must be text"),
    ("stream-long", _with(stream="x" * 97), "stream must be text"),
    ("run-bool", _with(run=True), "run must be a non-negative integer"),
    ("run-negative", _with(run=-1), "run must be a non-negative integer"),
    ("gen-string", _with(gen="6"), "gen must be a non-negative integer"),
    ("q-float", _with(q=1.5), "q must be a non-negative integer"),
    ("detail-missing-key", _detail_without("prev_q"), "detail must have exactly the keys"),
    ("detail-extra-key", _detail_with(x=1), "detail must have exactly the keys"),
    ("detail-list", _with(detail=[1, 2]), "detail must have exactly the keys"),
    (
        "detail-list-of-the-right-keys",
        _with(detail=["prev", "got", "missing", "prev_q"]),
        "detail must have exactly the keys",
    ),
    ("detail-float", _detail_with(got=1.5), "detail values must be integers"),
    ("detail-bool", _detail_with(got=True), "detail values must be integers"),
    ("reason-short", _with(reason="too short"), "reason must be text"),
    ("reason-number", _with(reason=12345), "reason must be text"),
    ("reason-blank", _with(reason=" " * 40), "reason must be text"),
    ("reason-huge", _with(reason="r" * 2001), "reason must be text"),
    ("evidence-blank", _with(evidence="   "), "evidence must be text"),
    ("by-empty", _with(by=""), "by must be text"),
    ("by-list", _with(by=["x"]), "by must be text"),
    ("at-date-only", _with(at="2026-10-19"), "at must be a UTC time"),
    ("at-number", _with(at=20261019), "at must be a UTC time"),
    ("at-offset", _with(at="2026-10-19T08:00:00+00:00"), "at must be a UTC time"),
    ("at-impossible", _with(at="2026-13-45T25:61:61Z"), "does not match format"),
]


@pytest.mark.parametrize(
    ("make", "error"), [(m, e) for _, m, e in BAD_LINES], ids=[n for n, _, _ in BAD_LINES]
)
def test_a_malformed_line_is_a_fail_and_hides_nothing(tmp_path, make, error):
    gap = gap_lake(tmp_path)
    line = make(ack_dict(gap))
    write_acks(tmp_path, line if isinstance(line, str) else json.dumps(line))
    rep = verify_lake(tmp_path)
    assert rep.exit_code == 1 and "AGGTRADE_ID_GAP" in codes(rep, "FAIL")
    bad = gap_of(rep, "ACK_INVALID")
    assert bad.detail["file"] == ACK_FILE and bad.detail["line"] == 1 and error in bad.detail["error"]
    assert rep.acks["entries"] == 0 and rep.acks["invalid"] == 1 and rep.acks["applied"] == []


def test_valid_lines_still_apply_next_to_a_bad_one_but_the_bad_one_keeps_the_lake_failing(tmp_path):
    build_clean(tmp_path, agg_skip={7, 15})
    gaps = [i for i in verify_lake(tmp_path).issues if i.code == "AGGTRADE_ID_GAP"]
    write_acks(tmp_path, "{broken", "", json.dumps(ack_dict(gaps[0], id="A")))
    rep = verify_lake(tmp_path)
    assert rep.exit_code == 1
    assert gap_of(rep, "ACK_INVALID").detail["line"] == 1
    assert rep.acks["applied"] == ["A"]
    assert sorted(i.severity for i in rep.issues if i.code == "AGGTRADE_ID_GAP") == ["FAIL", "INFO"]


def test_a_duplicate_id_or_a_second_entry_for_one_gap_is_rejected(tmp_path):
    build_clean(tmp_path, agg_skip={7, 15})
    gaps = [i for i in verify_lake(tmp_path).issues if i.code == "AGGTRADE_ID_GAP"]
    write_acks(tmp_path, json.dumps(ack_dict(gaps[0], id="A")), json.dumps(ack_dict(gaps[1], id="A")))
    rep = verify_lake(tmp_path)
    assert rep.exit_code == 1 and rep.acks["applied"] == ["A"]
    assert "duplicate id A" in gap_of(rep, "ACK_INVALID").detail["error"]
    write_acks(tmp_path, json.dumps(ack_dict(gaps[0], id="A")), json.dumps(ack_dict(gaps[0], id="B")))
    rep = verify_lake(tmp_path)
    assert rep.exit_code == 1 and rep.acks["applied"] == ["A"]
    assert "same gap" in gap_of(rep, "ACK_INVALID").detail["error"]


def test_an_unreadable_file_fails_instead_of_reading_as_empty(tmp_path):
    gap = gap_lake(tmp_path)
    (tmp_path / ACK_FILE).mkdir()  # a directory where the file should be
    rep = verify_lake(tmp_path)
    assert rep.exit_code == 1 and {"ACK_INVALID", "AGGTRADE_ID_GAP"} <= set(codes(rep, "FAIL"))
    (tmp_path / ACK_FILE).rmdir()
    (tmp_path / ACK_FILE).write_bytes(b"\xff\xfe" + json.dumps(ack_dict(gap)).encode())
    rep = verify_lake(tmp_path)
    assert rep.exit_code == 1 and "not UTF-8" in gap_of(rep, "ACK_INVALID").detail["error"]
    (tmp_path / ACK_FILE).write_bytes(b" " * (MAX_ACK_BYTES + 1))
    rep = verify_lake(tmp_path)
    assert rep.exit_code == 1 and "larger than" in gap_of(rep, "ACK_INVALID").detail["error"]


def test_a_pathologically_nested_line_is_a_fail_not_a_crash(tmp_path):
    gap_lake(tmp_path)
    write_acks(tmp_path, "[" * 200_000)
    rep = verify_lake(tmp_path)
    assert rep.exit_code == 1 and "nested too deeply" in gap_of(rep, "ACK_INVALID").detail["error"]


def test_a_windows_editor_file_with_bom_crlf_and_blank_lines_still_works(tmp_path):
    gap = gap_lake(tmp_path)
    body = "\r\n" + json.dumps(ack_dict(gap)) + "\r\n\r\n"
    (tmp_path / ACK_FILE).write_bytes(b"\xef\xbb\xbf" + body.encode())
    rep = verify_lake(tmp_path)
    assert rep.exit_code == 0 and rep.acks["applied"] == ["ACK-1"]


def test_an_explicit_ack_path_replaces_the_default_file(tmp_path):
    lake = tmp_path / "lake"
    gap = gap_lake(lake)
    elsewhere = tmp_path / "other-acks.jsonl"
    elsewhere.write_text(json.dumps(ack_dict(gap)) + "\n", encoding="utf-8")
    rep = verify_lake(lake, acks_path=elsewhere)
    assert rep.exit_code == 0 and rep.acks["file"] == elsewhere.name
    write_acks(lake, json.dumps(ack_dict(gap)))
    assert verify_lake(lake, acks_path=tmp_path / "does-not-exist.jsonl").exit_code == 1


# -- apply_acks on its own: the severity rule and single use are not reachable through a real lake ---------
def _ack(issue, ident="A"):
    d = ack_dict(issue, id=ident)
    d.pop("v")
    return Ack(**d, line=1)


def _issue(severity="FAIL", **over):
    base = {"prev": 5, "got": 9, "missing": 3, "prev_q": 40}
    return Issue("AGGTRADE_ID_GAP", severity, "usdm:btcusdt@aggTrade", 4, 50, 6, base | over)


@pytest.mark.parametrize("severity", ["WARN", "INFO"])
def test_only_a_fail_can_be_downgraded(severity):
    issue = _issue(severity)
    applied, unmatched = apply_acks([issue], [_ack(issue)])
    assert applied == [] and len(unmatched) == 1
    assert issue.severity == severity and issue.explained_by is None


def test_an_ack_is_used_at_most_once():
    first, second = _issue(), _issue()
    applied, unmatched = apply_acks([first, second], [_ack(first)])
    assert applied == ["A"] and unmatched == []
    assert (first.severity, second.severity) == ("INFO", "FAIL")
    assert first.explained_by == "ACK:A" and second.explained_by is None


def test_a_hand_built_ack_for_a_non_ackable_code_is_still_refused():
    detail = {"prev": 5, "got": 9, "missing": 3, "prev_q": 40}
    hole = Issue("SEQ_HOLE", "FAIL", "-", 4, 50, 6, detail)
    when = "2026-10-19T08:00:00Z"
    ack = Ack("A", "SEQ_HOLE", "-", 4, 6, 50, dict(detail), "r" * 30, "evidence", "me", when, 1)
    applied, unmatched = apply_acks([hole], [ack])
    assert applied == [] and unmatched == [ack] and hole.severity == "FAIL" and hole.explained_by is None


def test_a_different_detail_value_is_not_the_same_gap():
    issue = _issue()
    applied, _ = apply_acks([issue], [_ack(_issue(missing=2))])
    assert applied == [] and issue.severity == "FAIL"


def test_load_acks_maps_every_field_and_the_result_applies_to_the_matching_issue(tmp_path):
    issue = _issue()
    line = {**ack_dict(issue), "id": "M-1", "by": "me", "at": "2026-10-19T08:00:00Z", "evidence": "e"}
    path = write_acks(tmp_path, json.dumps(line))
    acks, problems = load_acks(path)
    assert problems == [] and len(acks) == 1
    a = acks[0]
    assert (a.id, a.code, a.stream, a.run, a.gen, a.q) == ("M-1", issue.code, issue.stream, 4, 6, 50)
    assert (a.detail, a.reason, a.evidence, a.by, a.at, a.line) == (
        issue.detail,
        line["reason"],
        "e",
        "me",
        "2026-10-19T08:00:00Z",
        1,
    )
    assert apply_acks([issue], acks) == (["M-1"], []) and issue.severity == "INFO"


def test_load_acks_on_a_missing_file_is_empty_and_silent(tmp_path):
    assert load_acks(tmp_path / "nope.jsonl") == ([], [])


# -- the entry points that read the file: CLI and the maintenance run ------------------------------------
def test_the_cli_reads_the_default_file_and_the_acks_flag(tmp_path, capsys):
    lake = tmp_path / "lake"
    gap = gap_lake(lake)
    assert main(["verify", "--root", str(lake)]) == 1
    capsys.readouterr()
    elsewhere = tmp_path / "other-acks.jsonl"
    elsewhere.write_text(json.dumps(ack_dict(gap)) + "\n", encoding="utf-8")
    assert main(["verify", "--root", str(lake), "--acks", str(elsewhere)]) == 0
    assert "acknowledged: 1 known gap(s): ACK-1" in capsys.readouterr().out
    write_acks(lake, json.dumps(ack_dict(gap)))
    assert main(["verify", "--root", str(lake), "--json"]) == 0
    assert json.loads(capsys.readouterr().out)["acks"]["applied"] == ["ACK-1"]


def test_the_maintenance_verify_passes_with_an_ack_and_shows_it_in_the_result_line(tmp_path):
    lake = tmp_path / "lake"
    gap = gap_lake(lake)
    cfg = MaintConfig(host="h", lake=lake, logs=tmp_path / "logs")
    assert do_verify(cfg)["ok"] is False
    write_acks(lake, json.dumps(ack_dict(gap)))
    out = do_verify(cfg)
    assert out["ok"] is True
    assert out["result"].startswith("RESULT: PASS (FAIL=0") and out["result"].endswith(" ACKED=1)")
    assert "acknowledged: 1 known gap(s): ACK-1" in out["text"]
