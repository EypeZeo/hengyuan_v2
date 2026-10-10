"""hostprobe: a read-only health probe built from fixed commands and files (fakes stand in for systemd)."""

from __future__ import annotations

import json
import os
from pathlib import Path

import pytest

from hy_recorder import hostprobe

NOW_S = 1_790_000_000.0
H = 3600 * 1_000_000  # one hour in microseconds


def _us(hours_ago: float) -> int:
    return int(NOW_S * 1_000_000 - hours_ago * H)


def _write_jsonl(path: Path, events: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("".join(json.dumps(e) + "\n" for e in events), encoding="utf-8")


def _seal(name: str, sha: str, hours_ago: float) -> dict:
    return {"ev": "seal", "name": name, "sha256": sha, "bytes": 1000, "wall_us": _us(hours_ago)}


def _prune(name: str, sha: str, hours_ago: float, *, acked: bool) -> dict:
    reason = "acked_expired" if acked else "evicted_unacked"
    return {
        "ev": "prune",
        "name": name,
        "sha256": sha,
        "reason": reason,
        "acked": acked,
        "wall_us": _us(hours_ago),
    }


def _touch(root: Path, name: str) -> None:
    p = root / name
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(b"x")


def test_lake_summary_counts_unacked_age_and_evictions(tmp_path):
    a, b, c = "a" * 64, "b" * 64, "c" * 64
    names = {k: "raw/spot/trade/20261003/%02d-00000%d.jsonl.zst" % (i, i) for i, k in enumerate("abcdef")}
    _write_jsonl(
        tmp_path / "manifest" / "20261003.jsonl",
        [
            _seal(names["a"], a, 50),  # acknowledged
            _seal(names["b"], b, 40),  # not acknowledged, the oldest of those
            _seal(names["c"], c, 2),
            _seal(names["d"], "d" * 64, 60),
            _prune(names["d"], "d" * 64, 1, acked=True),  # reclaimed after its ack
            _seal(names["e"], "e" * 64, 55),
            _prune(names["e"], "e" * 64, 3, acked=False),  # evicted without an ack, 3 h ago
        ],
    )
    for key in "abc":
        _touch(tmp_path, names[key])
    (tmp_path / "acks").mkdir()
    (tmp_path / "acks" / a).write_bytes(b"")
    lake = hostprobe._lake(str(tmp_path), NOW_S)
    assert lake["live_segments"] == 3 and lake["sealed_files_on_disk"] == 3
    assert lake["unacked_segments"] == 2 and lake["oldest_unacked_age_h"] == 40.0
    assert lake["prunes_total"] == 2
    assert lake["prunes_by_reason"] == {"acked_expired": 1, "evicted_unacked": 1}
    assert lake["evicted_unacked_total"] == 1 and lake["evicted_unacked_24h"] == 1
    assert lake["dangling_live"] == 0


def test_a_live_segment_whose_file_is_gone_is_dangling_and_an_old_eviction_is_not_recent(tmp_path):
    name = "raw/spot/trade/20261003/01-000001.jsonl.zst"
    _write_jsonl(
        tmp_path / "manifest" / "20261003.jsonl",
        [
            _seal(name, "a" * 64, 5),
            _prune("raw/x/y/20260901/00-000009.jsonl.zst", "f" * 64, 100, acked=False),
        ],
    )
    lake = hostprobe._lake(str(tmp_path), NOW_S)
    assert lake["dangling_live"] == 1
    assert lake["evicted_unacked_total"] == 1 and lake["evicted_unacked_24h"] == 0


def test_an_empty_lake_has_no_unacked_age(tmp_path):
    lake = hostprobe._lake(str(tmp_path), NOW_S)
    assert lake["live_segments"] == 0 and lake["oldest_unacked_age_h"] is None


def test_reconnect_gaps_are_close_to_open_per_connection_for_the_running_recorder_only(tmp_path):
    def close(run, q, hours_ago, conn, reason):
        return {"run": run, "q": q, "t": _us(hours_ago), "k": "WS_CLOSE", "conn": conn, "reason": reason}

    def opened(run, q, hours_ago, conn, plus_s):
        return {
            "run": run,
            "q": q,
            "t": _us(hours_ago) + int(plus_s * 1_000_000),
            "k": "WS_OPEN",
            "conn": conn,
        }

    events = [
        # run 1 (an earlier release): its 5.3 s rotation is no evidence about the running recorder
        close(1, 1, 20, "spot_trade", "rotation"),
        opened(1, 2, 20, "spot_trade", 5.3),
        {"run": 2, "q": 1, "t": _us(10), "k": "PROC_START", "version": "0.1.0", "prev_clean": True},
        # run 2: two connections rotate, interleaved: each is paired with ITS OWN next open
        close(2, 2, 6, "spot_trade", "rotation"),
        close(2, 3, 6, "usdm_market", "rotation"),
        opened(2, 4, 6, "usdm_market", 0.52),
        opened(2, 5, 6, "spot_trade", 0.41),
        close(2, 6, 3, "spot_trade", "stall:spot:btcusdt@trade"),
        opened(2, 7, 3, "spot_trade", 12.0),
        close(2, 8, 30, "spot_trade", "rotation"),  # more than 24 h ago: outside the window
        opened(2, 9, 30, "spot_trade", 0.3),
        close(2, 10, 1, "usdm_market", "rotation"),  # closed, not reopened yet: no gap to report
    ]
    _write_jsonl(tmp_path / "ledger" / "20261003.jsonl", events)
    gaps = hostprobe._ledger(str(tmp_path), NOW_S)["reconnect_gaps"]
    got = sorted((g["conn"], g["reason"], g["gap_s"]) for g in gaps)
    assert got == [
        ("spot_trade", "rotation", 0.41),
        ("spot_trade", "stall", 12.0),
        ("usdm_market", "rotation", 0.52),
    ]


def test_reconnect_gaps_survive_a_run_older_than_the_newest_three_ledger_files(tmp_path):
    # The probe reads the newest three daily files. A recorder that has been up for days no longer has its
    # PROC_START among them, so the running run must be told from the events themselves (the highest run
    # number seen), not from the start event: otherwise every gap is dropped and the rotation hole of a
    # long-running recorder is never measured (run 4 of 2026-10-07 reached this on 2026-10-10).
    def close(q, hours_ago, conn, reason):
        return {"run": 4, "q": q, "t": _us(hours_ago), "k": "WS_CLOSE", "conn": conn, "reason": reason}

    def opened(q, hours_ago, conn, plus_s):
        t = _us(hours_ago) + int(plus_s * 1_000_000)
        return {"run": 4, "q": q, "t": t, "k": "WS_OPEN", "conn": conn}

    ledger = tmp_path / "ledger"
    start = {"run": 4, "q": 1, "t": _us(100), "k": "PROC_START", "version": "0.1.0", "prev_clean": True}
    _write_jsonl(ledger / "20261001.jsonl", [start])
    _write_jsonl(
        ledger / "20261002.jsonl", [close(2, 60, "spot_trade", "rotation"), opened(3, 60, "spot_trade", 0.4)]
    )
    _write_jsonl(
        ledger / "20261003.jsonl", [close(4, 40, "spot_trade", "rotation"), opened(5, 40, "spot_trade", 0.4)]
    )
    _write_jsonl(
        ledger / "20261004.jsonl",
        [
            close(6, 5, "usdm_market", "closed:None"),
            opened(7, 5, "usdm_market", 0.31),
            close(8, 2, "spot_trade", "rotation"),
            opened(9, 2, "spot_trade", 0.52),
        ],
    )
    led = hostprobe._ledger(str(tmp_path), NOW_S)
    assert led["last_proc_start"] is None  # the start event really is outside the files read
    got = sorted((g["conn"], g["reason"], g["gap_s"]) for g in led["reconnect_gaps"])
    assert got == [("spot_trade", "rotation", 0.52), ("usdm_market", "closed", 0.31)]


def test_reconnect_gaps_of_an_earlier_run_are_dropped_when_a_newer_run_is_in_the_files(tmp_path):
    events = [
        {"run": 3, "q": 1, "t": _us(9), "k": "WS_CLOSE", "conn": "spot_trade", "reason": "rotation"},
        {"run": 3, "q": 2, "t": _us(9) + 5_300_000, "k": "WS_OPEN", "conn": "spot_trade"},
        {"run": 4, "q": 1, "t": _us(8), "k": "WS_CLOSE", "conn": "spot_trade", "reason": "closed:None"},
        {"run": 4, "q": 2, "t": _us(8) + 400_000, "k": "WS_OPEN", "conn": "spot_trade"},
    ]
    _write_jsonl(tmp_path / "ledger" / "20261003.jsonl", events)
    gaps = hostprobe._ledger(str(tmp_path), NOW_S)["reconnect_gaps"]
    assert [(g["run"], g["reason"], g["gap_s"]) for g in gaps] == [(4, "closed", 0.4)]


def test_ledger_summary_counts_only_the_last_24_hours_and_keeps_the_latest_clock_state(tmp_path):
    events = [
        {"run": 1, "q": 1, "t": _us(30), "k": "RATE_LIMIT"},  # too old
        {"run": 1, "q": 2, "t": _us(20), "k": "WS_CLOSE", "reason": "rotation"},
        {"run": 1, "q": 3, "t": _us(10), "k": "WS_CLOSE", "reason": "stall:spot:btcusdt@trade"},
        {"run": 1, "q": 4, "t": _us(9), "k": "RATE_LIMIT"},
        {"run": 1, "q": 5, "t": _us(9), "k": "CLOCK_PROBE"},  # not of interest
        {"run": 2, "q": 1, "t": _us(8), "k": "PROC_START", "version": "0.1.0", "prev_clean": True},
        {
            "run": 2,
            "q": 2,
            "t": _us(8),
            "k": "CLOCK_STATE",
            "synced": False,
            "reason": "no_marker",
            "initial": True,
        },
        {
            "run": 2,
            "q": 3,
            "t": _us(7),
            "k": "CLOCK_STATE",
            "synced": True,
            "unsynced_s": 36.0,
            "initial": False,
        },
    ]
    _write_jsonl(tmp_path / "ledger" / "20261003.jsonl", events)
    led = hostprobe._ledger(str(tmp_path), NOW_S)
    assert led["events_24h"] == {"WS_CLOSE": 2, "RATE_LIMIT": 1, "PROC_START": 1}
    assert led["ws_close_reasons_24h"] == {"rotation": 1, "stall": 1}
    assert led["last_clock_state"]["synced"] is True and led["last_clock_state"]["unsynced_s"] == 36.0
    assert led["last_proc_start"]["prev_clean"] is True and led["last_proc_stop"] is None


def _fake_run(outputs: dict[str, str]):
    def run(cmd: str) -> str:
        for needle, out in outputs.items():
            if needle in cmd:
                return out
        raise AssertionError("unexpected command: " + cmd)

    return run


def _cgroup(tmp_path: Path) -> Path:
    cg = tmp_path / "cg"
    cg.mkdir()
    (cg / "memory.stat").write_text("anon 138158080\nfile 237420544\nkernel 1\n")
    (cg / "memory.events").write_text("low 0\nhigh 2102\nmax 0\noom 0\noom_kill 0\n")
    (cg / "memory.current").write_text("418865152\n")
    return cg


OUTPUTS = {
    "systemctl show hy-recorder": (
        "ActiveState=active\nSubState=running\nNRestarts=2\nMainPID=777\nActiveEnterTimestamp=x"
    ),
    "is-active systemd-timesyncd": "active",
    "is-active x-ui": "active",
    "is-active fail2ban": "active",
    "pgrep -c xray": "1",
    "journalctl --disk-usage": "Archived and active journals take up 22.5M in the file system.",
}


def test_collect_assembles_every_section_from_fixed_commands_and_files(tmp_path):
    lake = tmp_path / "lake"
    lake.mkdir()
    marker = tmp_path / "synchronized"
    marker.write_bytes(b"")
    os.utime(marker, (NOW_S - 120, NOW_S - 120))
    jdir = tmp_path / "journal"
    jdir.mkdir()
    out = hostprobe.collect(
        str(lake),
        run=_fake_run(OUTPUTS),
        now=lambda: NOW_S,
        cgroup=str(_cgroup(tmp_path)),
        marker=str(marker),
        journal_dir=str(jdir),
    )
    assert out["errors"] == [] and out["schema"] == 1
    assert out["service"] == {
        "ActiveState": "active",
        "SubState": "running",
        "NRestarts": 2,
        "MainPID": 777,
        "ActiveEnterTimestamp": "x",
    }
    assert out["disk"]["total_bytes"] > 0 and 0 <= out["disk"]["used_pct"] <= 100
    assert out["clock"] == {"timesyncd": "active", "marker_present": True, "marker_age_s": 120.0}
    assert out["memory"]["anon_bytes"] == 138158080 and out["memory"]["events"]["high"] == 2102
    assert out["journal"] == {"persistent_dir": True, "disk_usage_bytes": int(22.5 * 2**20)}
    assert out["co_tenants"] == {"x-ui": "active", "fail2ban": "active", "xray_procs": 1}
    json.dumps(out)  # the whole thing must be serialisable: it is printed as one JSON line


def test_a_failing_sub_probe_is_recorded_and_the_others_still_run(tmp_path):
    lake = tmp_path / "lake"
    lake.mkdir()

    def run(cmd: str) -> str:
        if "systemctl show" in cmd:
            raise OSError("systemctl missing")
        return _fake_run(OUTPUTS)(cmd)

    out = hostprobe.collect(
        str(lake),
        run=run,
        now=lambda: NOW_S,
        cgroup=str(_cgroup(tmp_path)),
        marker=str(tmp_path / "none"),
        journal_dir=str(tmp_path / "nojournal"),
    )
    assert out["service"] is None and any(e.startswith("service: OSError") for e in out["errors"])
    assert out["disk"] is not None and out["co_tenants"]["x-ui"] == "active"
    assert out["clock"]["marker_present"] is False and out["journal"]["persistent_dir"] is False


@pytest.mark.parametrize(
    ("text", "expected"),
    [
        ("Archived and active journals take up 22.5M in the file system.", int(22.5 * 2**20)),
        ("Archived and active journals take up 1.2G in the file system.", int(1.2 * 2**30)),
        ("Archived and active journals take up 512.0K in the file system.", 512 * 2**10),
        ("nothing parseable here", None),
    ],
)
def test_journal_usage_parsing(tmp_path, text, expected):
    out = hostprobe._journal(lambda cmd: text, str(tmp_path))
    assert out["disk_usage_bytes"] == expected
