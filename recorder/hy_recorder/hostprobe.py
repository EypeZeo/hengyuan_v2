#!/usr/bin/env python3
"""Read-only health probe of the D0 recorder host.

Run there from the operator's machine (``ssh host python3 - < hostprobe.py``, see ``maint.py``); prints one
JSON object. It never writes anything, imports nothing but the standard library (the host has no pip and
``hy_recorder`` is not importable from a bare ``python3 -``), and a sub-probe that fails is recorded in
``errors`` instead of hiding the rest.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import time
from collections import Counter
from collections.abc import Callable
from pathlib import Path
from typing import Any

US = 1_000_000
ROOT = "/var/lib/hy-recorder"
UNIT = "hy-recorder"
CGROUP = "/sys/fs/cgroup/system.slice/hy-recorder.service"
SYNC_MARKER = "/run/systemd/timesync/synchronized"
JOURNAL_DIR = "/var/log/journal"
# ledger kinds worth counting over the last 24 h (everything else is noise for a health check)
INTEREST = (
    "WS_OPEN",
    "WS_CLOSE",
    "WS_CONNECT_FAIL",
    "STREAM_STALL",
    "SUBSCRIBED_NO_DATA",
    "GAP_DETECTED",
    "BAD_FRAME",
    "OVERRUN",
    "RATE_LIMIT",
    "REST_FROZEN",
    "BAN",
    "DISK_WARN",
    "DISK_STOP",
    "EVICTED_UNACKED",
    "PRUNED_ACKED",
    "RETENTION_STEPDOWN",
    "TASK_CRASH",
    "LOOP_LAG",
    "PROC_START",
    "PROC_STOP",
)

Runner = Callable[[str], str]


def sh(cmd: str, timeout: float = 20.0) -> str:
    return subprocess.run(
        cmd, shell=True, capture_output=True, text=True, timeout=timeout, check=False
    ).stdout.strip()


def _read(path: str) -> str:
    try:
        return Path(path).read_text().strip()
    except OSError:
        return ""


def _jsonl(path: Path) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    try:
        data = path.read_bytes()
    except OSError:
        return out
    for line in data.splitlines():
        if line.strip():
            try:
                out.append(json.loads(line))
            except ValueError:
                pass
    return out


def _service(run: Runner) -> dict[str, Any]:
    keys = "ActiveState,SubState,NRestarts,MainPID,ActiveEnterTimestamp"
    out: dict[str, Any] = {}
    for line in run("systemctl show %s -p %s" % (UNIT, keys)).splitlines():
        k, _, v = line.partition("=")
        out[k] = v
    out["NRestarts"] = int(out["NRestarts"]) if str(out.get("NRestarts", "")).isdigit() else None
    out["MainPID"] = int(out["MainPID"]) if str(out.get("MainPID", "")).isdigit() else None
    return out


def _disk(root: str) -> dict[str, Any]:
    total, used, avail = shutil.disk_usage(root)  # free space as an unprivileged writer sees it
    return {
        "total_bytes": total,
        "avail_bytes": avail,
        "used_bytes": used,
        "used_pct": round(100.0 * used / total, 1) if total else None,
    }


def _lake(root: str, now_s: float) -> dict[str, Any]:
    rootp = Path(root)
    seals: dict[str, dict[str, Any]] = {}
    pruned: set[str] = set()
    prunes: list[dict[str, Any]] = []
    for fp in sorted((rootp / "manifest").glob("*.jsonl")):
        for ev in _jsonl(fp):
            if ev.get("ev") == "seal":
                seals[ev["name"]] = ev
                pruned.discard(ev["name"])
            elif ev.get("ev") == "prune":
                pruned.add(ev["name"])
                prunes.append(ev)
    live = {n: e for n, e in seals.items() if n not in pruned}
    acks_dir = rootp / "acks"
    acked = set(os.listdir(acks_dir)) if acks_dir.is_dir() else set()
    unacked = [e for e in live.values() if e.get("sha256") not in acked]
    oldest = min((int(e["wall_us"]) for e in unacked if isinstance(e.get("wall_us"), int)), default=None)
    day_ago_us = int((now_s - 86400) * US)
    return {
        "live_segments": len(live),
        "live_bytes": sum(int(e.get("bytes") or 0) for e in live.values()),
        "sealed_files_on_disk": sum(1 for _ in (rootp / "raw").rglob("*.jsonl.zst")),
        "part_files": sum(1 for _ in (rootp / "raw").rglob("*.part")),
        "dangling_live": sum(1 for n in live if not (rootp / n).exists()),
        "unacked_segments": len(unacked),
        "oldest_unacked_age_h": None if oldest is None else round((now_s * US - oldest) / US / 3600, 1),
        "prunes_total": len(prunes),
        "prunes_by_reason": dict(Counter(str(p.get("reason")) for p in prunes)),
        "evicted_unacked_total": sum(1 for p in prunes if not p.get("acked")),
        "evicted_unacked_24h": sum(
            1 for p in prunes if not p.get("acked") and int(p.get("wall_us") or 0) >= day_ago_us
        ),
    }


def _brief(event: dict[str, Any] | None, *keys: str) -> dict[str, Any] | None:
    return None if event is None else {k: event.get(k) for k in ("run", "t", *keys)}


def _ledger(root: str, now_s: float) -> dict[str, Any]:
    ledger_dir = Path(root) / "ledger"
    files = sorted(ledger_dir.glob("*.jsonl"))[-3:]  # today, yesterday and the day before: enough for 24 h
    cutoff_us = int((now_s - 86400) * US)
    counts: Counter[str] = Counter()
    close_reasons: Counter[str] = Counter()
    last_clock_state: dict[str, Any] | None = None
    last_start: dict[str, Any] | None = None
    last_stop: dict[str, Any] | None = None
    closed: dict[
        tuple[Any, Any], tuple[int, str]
    ] = {}  # (run, connection) -> (time, reason) of its last close
    gaps: list[dict[str, Any]] = []
    for fp in files:
        for ev in _jsonl(fp):
            k, t = ev.get("k"), ev.get("t")
            if k == "CLOCK_STATE":
                last_clock_state = ev
            elif k == "PROC_START":
                last_start = ev
            elif k == "PROC_STOP":
                last_stop = ev
            elif k == "WS_CLOSE" and isinstance(t, int):
                closed[(ev.get("run"), ev.get("conn"))] = (t, str(ev.get("reason", "?")).split(":")[0])
            elif k == "WS_OPEN" and isinstance(t, int) and (ev.get("run"), ev.get("conn")) in closed:
                t0, why = closed.pop((ev.get("run"), ev.get("conn")))
                gaps.append(
                    {
                        "run": ev.get("run"),
                        "conn": ev.get("conn"),
                        "reason": why,
                        "t": t,
                        "gap_s": round((t - t0) / US, 3),
                    }
                )
            if not isinstance(t, int) or t < cutoff_us or k not in INTEREST:
                continue
            counts[k] += 1
            if k == "WS_CLOSE":
                close_reasons[str(ev.get("reason", "?")).split(":")[0]] += 1
    current_run = (last_start or {}).get("run")
    return {
        "events_24h": dict(counts),
        "ws_close_reasons_24h": dict(close_reasons),
        # WS_CLOSE -> next WS_OPEN of the same connection, this run only (an earlier release's rotations are
        # not evidence about the running one), last 24 h
        "reconnect_gaps": [g for g in gaps if g["run"] == current_run and g["t"] >= cutoff_us],
        "last_clock_state": _brief(last_clock_state, "synced", "reason", "unsynced_s", "initial"),
        "last_proc_start": _brief(last_start, "version", "prev_clean"),
        "last_proc_stop": _brief(last_stop, "reason"),
    }


def _clock(run: Runner, now_s: float, marker: str) -> dict[str, Any]:
    try:
        age = round(max(0.0, now_s - os.stat(marker).st_mtime), 1)
    except OSError:
        age = None
    return {
        "timesyncd": run("systemctl is-active systemd-timesyncd"),
        "marker_present": age is not None,
        "marker_age_s": age,
    }


def _memory(cgroup: str, pid: int | None) -> dict[str, Any]:
    stat = dict(
        line.split() for line in _read(cgroup + "/memory.stat").splitlines() if len(line.split()) == 2
    )
    events = dict(
        line.split() for line in _read(cgroup + "/memory.events").splitlines() if len(line.split()) == 2
    )
    out: dict[str, Any] = {
        "anon_bytes": int(stat["anon"]) if "anon" in stat else None,
        "file_bytes": int(stat["file"]) if "file" in stat else None,
        "current_bytes": int(_read(cgroup + "/memory.current") or 0) or None,
        "events": {k: int(v) for k, v in events.items()},
    }
    if pid:
        try:
            out["threads"] = len(os.listdir("/proc/%d/task" % pid))
            out["fds"] = len(os.listdir("/proc/%d/fd" % pid))
        except OSError:
            pass
    return out


def _journal(run: Runner, directory: str) -> dict[str, Any]:
    text = run("journalctl --disk-usage")
    size = None
    for tok in text.split():
        unit = tok[-1:]
        if unit in "KMGT" and tok[:-1].replace(".", "", 1).isdigit():
            size = int(float(tok[:-1]) * {"K": 1 << 10, "M": 1 << 20, "G": 1 << 30, "T": 1 << 40}[unit])
    return {"persistent_dir": os.path.isdir(directory), "disk_usage_bytes": size}


def _cotenants(run: Runner) -> dict[str, Any]:
    return {
        "x-ui": run("systemctl is-active x-ui"),
        "fail2ban": run("systemctl is-active fail2ban"),
        "xray_procs": int(run("pgrep -c xray") or 0),
    }


def collect(
    root: str = ROOT,
    *,
    run: Runner = sh,
    now: Callable[[], float] = time.time,
    cgroup: str = CGROUP,
    marker: str = SYNC_MARKER,
    journal_dir: str = JOURNAL_DIR,
) -> dict[str, Any]:
    now_s = now()
    out: dict[str, Any] = {
        "schema": 1,
        "now_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(now_s)),
        "boot_id": _read("/proc/sys/kernel/random/boot_id")[:8],
        "uptime_s": float((_read("/proc/uptime") or "0").split()[0]),
        "errors": [],
    }
    parts: list[tuple[str, Callable[[], Any]]] = [
        ("service", lambda: _service(run)),
        ("disk", lambda: _disk(root)),
        ("lake", lambda: _lake(root, now_s)),
        ("ledger", lambda: _ledger(root, now_s)),
        ("clock", lambda: _clock(run, now_s, marker)),
        ("memory", lambda: _memory(cgroup, (out.get("service") or {}).get("MainPID"))),
        ("journal", lambda: _journal(run, journal_dir)),
        ("co_tenants", lambda: _cotenants(run)),
    ]
    for name, fn in parts:
        try:
            out[name] = fn()
        except Exception as exc:  # noqa: BLE001 - one failing sub-probe must not hide the others
            out[name] = None
            out["errors"].append("%s: %s: %s" % (name, type(exc).__name__, str(exc)[:120]))
    return out


if __name__ == "__main__":
    json.dump(collect(sys.argv[1] if len(sys.argv) > 1 else ROOT), sys.stdout, separators=(",", ":"))
    sys.stdout.write("\n")
