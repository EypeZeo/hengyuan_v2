"""Periodic maintenance: the judgements (``evaluate``) and one full run against fake collaborators."""

from __future__ import annotations

import copy
import json
import os
import subprocess
from datetime import UTC, datetime, timedelta
from pathlib import Path

import pytest

from hy_recorder import maint
from hy_recorder.cli import build_parser
from hy_recorder.maint import CRIT, GB, INFO, MB, OK, WARN, MaintConfig, Thresholds, evaluate, overall

NOW = datetime(2026, 10, 3, 22, 0, tzinfo=UTC)
NOW_US = int(NOW.timestamp() * 1e6)


def cfg_for(tmp_path: Path, **kw) -> MaintConfig:
    return MaintConfig(host="hostalias", lake=tmp_path / "lake", logs=tmp_path / "logs", **kw)


def good_probe() -> dict:
    return {
        "schema": 1,
        "boot_id": "abcd1234",
        "uptime_s": 86400.0,
        "errors": [],
        "service": {"ActiveState": "active", "SubState": "running", "NRestarts": 0, "MainPID": 777},
        "disk": {"used_pct": 22.0},
        "lake": {
            "live_segments": 300,
            "unacked_segments": 5,
            "oldest_unacked_age_h": 3.0,
            "evicted_unacked_24h": 0,
            "dangling_live": 0,
        },
        "ledger": {
            "events_24h": {"WS_CLOSE": 4},
            "last_clock_state": {"run": 2, "t": NOW_US - 3600 * 10**6, "synced": True},
        },
        "clock": {"timesyncd": "active", "marker_present": True, "marker_age_s": 600.0},
        "memory": {"anon_bytes": 150 * MB, "events": {"oom_kill": 0}},
        "journal": {"persistent_dir": True, "disk_usage_bytes": 22 * MB},
        "co_tenants": {"x-ui": "active", "fail2ban": "active", "xray_procs": 1},
    }


def good_pull() -> dict:
    return {
        "ok": True,
        "fetched": 14,
        "bytes": 30_000_000,
        "already": 300,
        "acked": 14,
        "errors": [],
        "lost": [],
    }


def ev(tmp_path: Path, **over):
    args = {
        "pull": good_pull(),
        "pull_error": None,
        "probe": good_probe(),
        "probe_error": None,
        "verify": None,
        "local_free_bytes": 200 * GB,
        "state": {},
        "cfg": cfg_for(tmp_path),
        "now": NOW,
    }
    args.update(over)
    return evaluate(**args)


def codes(findings) -> list[tuple[str, str]]:
    return [(f.level, f.code) for f in findings]


def with_probe(**sections):
    p = copy.deepcopy(good_probe())
    for key, patch in sections.items():
        if isinstance(patch, dict) and isinstance(p.get(key), dict):
            p[key].update(patch)
        else:
            p[key] = patch
    return p


def test_a_healthy_run_has_no_findings(tmp_path):
    findings = ev(tmp_path)
    assert findings == [] and overall(findings) == OK


@pytest.mark.parametrize(
    ("over", "expected"),
    [
        ({"pull_error": "boom"}, [(WARN, "PULL_FAILED")]),
        ({"pull_error": "boom", "state": {"consecutive_pull_failures": 1}}, [(CRIT, "PULL_FAILED")]),
        ({"pull": {**good_pull(), "errors": ["seg x: sha mismatch"], "ok": False}}, [(CRIT, "PULL_ERRORS")]),
        ({"pull": {**good_pull(), "lost": ["raw/a"], "ok": False}}, [(CRIT, "PULL_LOST")]),
        ({"probe": None, "probe_error": "ssh exit 255"}, [(WARN, "HOST_UNREACHABLE")]),
        (
            {"probe": None, "probe_error": "x", "state": {"consecutive_probe_failures": 1}},
            [(CRIT, "HOST_UNREACHABLE")],
        ),
        (
            {"probe": with_probe(service={"ActiveState": "failed", "SubState": "failed"})},
            [(CRIT, "RECORDER_INACTIVE")],
        ),
        (
            {"probe": with_probe(service={"NRestarts": 3}), "state": {"boot_id": "abcd1234", "nrestarts": 1}},
            [(WARN, "RECORDER_RESTARTED")],
        ),
        (  # a reboot resets the restart counter: not a recorder restart, just a note
            {"probe": with_probe(service={"NRestarts": 0}), "state": {"boot_id": "old", "nrestarts": 5}},
            [(INFO, "HOST_REBOOTED")],
        ),
        ({"probe": with_probe(disk={"used_pct": 72.0})}, [(WARN, "HOST_DISK")]),
        ({"probe": with_probe(disk={"used_pct": 90.0})}, [(CRIT, "HOST_DISK")]),
        ({"probe": with_probe(lake={"evicted_unacked_24h": 2})}, [(CRIT, "EVICTED_UNACKED")]),
        ({"probe": with_probe(lake={"dangling_live": 1})}, [(CRIT, "MANIFEST_DANGLING")]),
        ({"probe": with_probe(lake={"oldest_unacked_age_h": 40.0})}, [(WARN, "UNACKED_BACKLOG")]),
        ({"probe": with_probe(lake={"oldest_unacked_age_h": 100.0})}, [(CRIT, "UNACKED_BACKLOG")]),
        ({"probe": with_probe(ledger={"events_24h": {"DISK_STOP": 1}})}, [(CRIT, "DISK_STOP")]),
        ({"probe": with_probe(ledger={"events_24h": {"BAN": 1}})}, [(CRIT, "BAN")]),
        ({"probe": with_probe(ledger={"events_24h": {"DISK_WARN": 2}})}, [(WARN, "DISK_WARN")]),
        ({"probe": with_probe(ledger={"events_24h": {"RATE_LIMIT": 1}})}, [(WARN, "RATE_LIMIT")]),
        ({"probe": with_probe(ledger={"events_24h": {"TASK_CRASH": 1}})}, [(WARN, "TASK_CRASH")]),
        ({"probe": with_probe(ledger={"events_24h": {"OVERRUN": 5}})}, [(WARN, "OVERRUN")]),
        ({"probe": with_probe(clock={"marker_age_s": 4 * 3600.0})}, [(WARN, "CLOCK_STALE")]),
        ({"probe": with_probe(memory={"anon_bytes": 350 * MB})}, [(WARN, "MEMORY_HIGH")]),
        ({"probe": with_probe(memory={"events": {"oom_kill": 1}})}, [(CRIT, "OOM_KILL")]),
        ({"probe": with_probe(journal={"persistent_dir": False})}, [(WARN, "JOURNAL_VOLATILE")]),
        ({"probe": with_probe(journal={"disk_usage_bytes": 300 * MB})}, [(WARN, "JOURNAL_BIG")]),
        ({"probe": with_probe(co_tenants={"x-ui": "inactive"})}, [(WARN, "COTENANT_DOWN")]),
        ({"probe": with_probe(co_tenants={"xray_procs": 0})}, [(WARN, "COTENANT_DOWN")]),
        ({"verify": {"ok": True, "result": "RESULT: PASS (FAIL=0 WARN=1 INFO=2)"}}, [(INFO, "VERIFY_OK")]),
        ({"verify": {"ok": False, "result": "RESULT: FAIL (FAIL=3 WARN=0 INFO=0)"}}, [(CRIT, "VERIFY_FAIL")]),
        ({"local_free_bytes": 30 * GB}, [(WARN, "LOCAL_DISK_LOW")]),
        ({"local_free_bytes": 10 * GB}, [(CRIT, "LOCAL_DISK_LOW")]),
    ],
)
def test_single_faults_map_to_one_finding(tmp_path, over, expected):
    assert codes(ev(tmp_path, **over)) == expected


def test_a_clock_that_has_been_unsynchronised_for_a_while_is_flagged_but_a_fresh_boot_is_not(tmp_path):
    long_unsynced = with_probe(
        ledger={
            "last_clock_state": {
                "run": 2,
                "t": NOW_US - 20 * 60 * 10**6,
                "synced": False,
                "reason": "no_marker",
            }
        }
    )
    assert codes(ev(tmp_path, probe=long_unsynced)) == [(WARN, "CLOCK_UNSYNCED")]
    fresh = with_probe(
        ledger={
            "last_clock_state": {"run": 2, "t": NOW_US - 30 * 10**6, "synced": False, "reason": "no_marker"}
        }
    )
    assert ev(tmp_path, probe=fresh) == []


def test_partial_probe_failures_are_only_an_info(tmp_path):
    p = with_probe(errors=["journal: OSError: x"])
    assert codes(ev(tmp_path, probe=p)) == [(INFO, "PROBE_PARTIAL")]


def test_no_success_for_two_days_while_this_run_also_fails_is_critical(tmp_path):
    stale = (NOW - timedelta(hours=60)).isoformat()
    failing = ev(tmp_path, pull_error="boom", state={"last_success_utc": stale})
    assert (CRIT, "NO_RECENT_SUCCESS") in codes(failing)
    healthy = ev(tmp_path, state={"last_success_utc": stale})
    assert healthy == []


def test_overall_is_the_worst_level():
    f = maint.Finding
    assert overall([]) == OK and overall([f(INFO, "A", "")]) == OK
    assert overall([f(INFO, "A", ""), f(WARN, "B", "")]) == WARN
    assert overall([f(WARN, "B", ""), f(CRIT, "C", "")]) == CRIT


# -- a whole run with fake collaborators -----------------------------------------------------------------


class Fakes:
    def __init__(self):
        self.calls: list[str] = []
        self.pull_result = good_pull()
        self.pull_exc: Exception | None = None
        self.probe_result = good_probe()
        self.probe_exc: Exception | None = None
        self.verify_result = {
            "ok": True,
            "result": "RESULT: PASS (FAIL=0 WARN=1 INFO=2)",
            "text": "verify text\n",
        }

    def pull(self, cfg, log):
        self.calls.append("pull")
        log("fetched one segment")
        if self.pull_exc:
            raise self.pull_exc
        return self.pull_result

    def probe(self, cfg):
        self.calls.append("probe")
        if self.probe_exc:
            raise self.probe_exc
        return self.probe_result

    def verify(self, cfg):
        self.calls.append("verify")
        return self.verify_result

    def report(self, cfg):
        self.calls.append("report")
        return "report text\n"


def run(tmp_path, fakes, *, now=NOW, force=False, **kw):
    cfg = cfg_for(tmp_path, **kw)
    cfg.lake.mkdir(exist_ok=True)
    return cfg, maint.run_maintenance(
        cfg,
        force=force,
        now=now,
        pull_fn=fakes.pull,
        probe_fn=fakes.probe,
        verify_fn=fakes.verify,
        report_fn=fakes.report,
        free_fn=lambda p: 200 * GB,
        log=lambda m: None,
    )


def test_a_healthy_run_writes_the_status_files_and_the_first_run_also_verifies(tmp_path):
    fakes = Fakes()
    cfg, code = run(tmp_path, fakes)
    assert code == 0 and fakes.calls == ["pull", "probe", "verify", "report"]
    logs = cfg.logs
    status = json.loads((logs / "status.json").read_text())
    assert status["level"] == OK and status["findings"][0]["code"] == "VERIFY_OK"
    assert "level OK" in (logs / "latest.txt").read_text()
    stamp = NOW.strftime("%Y%m%d-%H%M%S")
    assert (logs / f"verify-{stamp}.txt").read_text() == "verify text\n"
    assert (logs / f"report-{stamp}.txt").read_text() == "report text\n"
    assert (logs / f"maint-{stamp}.log").exists() and not (logs / "maint.lock").exists()
    state = json.loads((logs / "state.json").read_text())
    assert state["last_success_utc"] == NOW.isoformat() and state["last_verify_utc"] == NOW.isoformat()
    assert state["boot_id"] == "abcd1234" and state["nrestarts"] == 0


def test_a_catch_up_trigger_right_after_a_success_does_nothing_unless_forced(tmp_path):
    fakes = Fakes()
    run(tmp_path, fakes)
    fakes.calls.clear()
    _, code = run(tmp_path, fakes, now=NOW + timedelta(hours=1))
    assert code == 0 and fakes.calls == []
    _, code = run(tmp_path, fakes, now=NOW + timedelta(hours=1), force=True)
    assert code == 0 and fakes.calls == ["pull", "probe"], "the weekly verify is not due again after an hour"
    fakes.calls.clear()
    run(tmp_path, fakes, now=NOW + timedelta(days=8))
    assert fakes.calls == ["pull", "probe", "verify", "report"]


def test_consecutive_failures_escalate_from_warn_to_crit_and_recovery_resets_them(tmp_path):
    fakes = Fakes()
    run(tmp_path, fakes)
    fakes.pull_exc, fakes.probe_exc = RuntimeError("ssh exit 255"), RuntimeError("ssh exit 255")
    t1 = NOW + timedelta(hours=7)
    _, code = run(tmp_path, fakes, now=t1)
    assert code == 1
    _, code = run(tmp_path, fakes, now=t1 + timedelta(hours=7))
    assert code == 2
    state = json.loads((tmp_path / "logs" / "state.json").read_text())
    assert state["consecutive_pull_failures"] == 2 and state["last_success_utc"] == NOW.isoformat()
    fakes.pull_exc = fakes.probe_exc = None
    _, code = run(tmp_path, fakes, now=t1 + timedelta(hours=14))
    state = json.loads((tmp_path / "logs" / "state.json").read_text())
    assert code == 0 and state["consecutive_pull_failures"] == 0 and state["consecutive_probe_failures"] == 0


def test_a_critical_finding_exits_2_and_does_not_count_as_a_success(tmp_path):
    fakes = Fakes()
    fakes.probe_result = with_probe(lake={"evicted_unacked_24h": 1})
    _, code = run(tmp_path, fakes)
    state = json.loads((tmp_path / "logs" / "state.json").read_text())
    assert code == 2 and "last_success_utc" not in state


def test_a_failed_verify_is_critical_and_is_retried_on_the_next_run(tmp_path):
    fakes = Fakes()
    fakes.verify_result = {"ok": False, "result": "RESULT: FAIL (FAIL=3 WARN=0 INFO=0)", "text": "bad\n"}
    _, code = run(tmp_path, fakes)
    assert code == 2
    state = json.loads((tmp_path / "logs" / "state.json").read_text())
    assert "last_verify_utc" not in state
    fakes.verify_result = {"ok": True, "result": "RESULT: PASS (FAIL=0 WARN=0 INFO=0)", "text": "ok\n"}
    fakes.calls.clear()
    run(tmp_path, fakes, now=NOW + timedelta(hours=7))
    assert "verify" in fakes.calls


def test_a_second_run_is_refused_while_the_lock_is_fresh_and_a_stale_lock_is_taken_over(tmp_path):
    fakes = Fakes()
    logs = tmp_path / "logs"
    logs.mkdir()
    lock = logs / "maint.lock"
    lock.write_text("123\n")
    now = datetime.fromtimestamp(lock.stat().st_mtime + 60, UTC)
    _, code = run(tmp_path, fakes, now=now)
    assert code == 0 and fakes.calls == [] and lock.exists()
    os.utime(lock, (0, 0))  # a crashed run left it behind long ago
    _, code = run(tmp_path, fakes, now=now)
    assert code == 0 and fakes.calls[:2] == ["pull", "probe"] and not lock.exists()


def test_error_text_never_leaks_the_host_address_into_the_files(tmp_path):
    fakes = Fakes()
    fakes.pull_exc = RuntimeError("Connection closed by 203.0.113.5 port 22")
    fakes.probe_exc = RuntimeError("ssh/probe exit 255: connect to host 203.0.113.5 port 22: timed out")
    cfg, _ = run(tmp_path, fakes)
    for name in ("status.json", "latest.txt"):
        text = (cfg.logs / name).read_text()
        assert "203.0.113.5" not in text and "<ip>" in text
    assert "203.0.113.5" not in "".join(p.read_text() for p in cfg.logs.glob("maint-*.log"))


def test_old_logs_are_removed_and_the_current_ones_are_kept(tmp_path):
    fakes = Fakes()
    logs = tmp_path / "logs"
    logs.mkdir()
    old_log, old_report, keep = (
        logs / "maint-20250101-000000.log",
        logs / "report-20250101-000000.txt",
        logs / "maint-new.log",
    )
    for p in (old_log, old_report, keep):
        p.write_text("x")
    long_ago, recent = NOW.timestamp() - 100 * 86400, NOW.timestamp() - 86400
    os.utime(old_log, (long_ago, long_ago))
    os.utime(old_report, (long_ago, long_ago))
    os.utime(keep, (recent, recent))  # explicit: the test must not depend on the real date
    run(tmp_path, fakes, thresholds=Thresholds(log_keep_days=60))
    assert not old_log.exists() and not old_report.exists() and keep.exists()
    assert (logs / "latest.txt").exists() and (logs / "state.json").exists()


def test_verify_that_cannot_run_is_a_critical_finding_not_a_crash(tmp_path):
    fakes = Fakes()

    def broken(cfg):
        raise OSError("lake unreadable")

    fakes.verify = broken
    _, code = run(tmp_path, fakes)
    status = json.loads((tmp_path / "logs" / "status.json").read_text())
    assert code == 2 and any(
        f["code"] == "VERIFY_FAIL" and "could not run" in f["msg"] for f in status["findings"]
    )


# -- the real probe transport and the config ---------------------------------------------------------------


def _proc(returncode, stdout=b"", stderr=b""):
    return subprocess.CompletedProcess([], returncode, stdout, stderr)


def test_the_probe_retries_a_failed_connection_and_returns_the_last_json_line(tmp_path):
    seen: list[list[str]] = []
    results = iter(
        [_proc(255, stderr=b"Connection closed"), _proc(0, stdout=b'noise\n{"schema": 1, "boot_id": "x"}\n')]
    )

    def fake_run(argv, **kw):
        seen.append(argv)
        assert kw["input"].startswith(b"#!/usr/bin/env python3") and b"def collect(" in kw["input"]
        return next(results)

    slept: list[float] = []
    out = maint.do_probe(cfg_for(tmp_path), run=fake_run, sleep=slept.append)
    assert out == {"schema": 1, "boot_id": "x"} and slept == [5.0] and len(seen) == 2
    assert seen[0][0] == "ssh" and "BatchMode=yes" in seen[0] and "hostalias" in seen[0]
    assert seen[0][-1].startswith("nice -n 19 python3 - ") and "/var/lib/hy-recorder" in seen[0][-1]


def test_a_probe_that_keeps_failing_raises_with_the_address_redacted(tmp_path):
    def fake_run(argv, **kw):
        return _proc(255, stderr=b"ssh: connect to host 198.51.100.7 port 22: Connection timed out\n")

    with pytest.raises(RuntimeError) as exc:
        maint.do_probe(cfg_for(tmp_path), run=fake_run, sleep=lambda s: None)
    assert "198.51.100.7" not in str(exc.value) and "<ip>" in str(exc.value) and "exit 255" in str(exc.value)


def test_load_config_reads_json_and_rejects_unknown_thresholds(tmp_path):
    good = {
        "host": "tokyo-vps-8t",
        "lake": str(tmp_path / "lake"),
        "logs": str(tmp_path / "logs"),
        "thresholds": {"local_free_warn_gb": 50},
    }
    p = tmp_path / "maint.json"
    p.write_text(json.dumps(good))
    cfg = maint.load_config(p)
    assert (
        cfg.host == "tokyo-vps-8t"
        and cfg.lake == tmp_path / "lake"
        and cfg.thresholds.local_free_warn_gb == 50
    )
    assert cfg.thresholds.local_free_crit_gb == Thresholds().local_free_crit_gb
    good["thresholds"] = {"local_free_warn": 50}
    p.write_text(json.dumps(good))
    with pytest.raises(ValueError, match="unknown thresholds"):
        maint.load_config(p)


def test_the_maint_subcommand_is_wired_into_the_cli():
    args = build_parser().parse_args(["maint", "--config", "x.json", "--force"])
    assert args.cmd == "maint" and args.force is True and callable(args.fn)
