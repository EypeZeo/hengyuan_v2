"""Operator-side periodic maintenance of the D0 recorder (run by a scheduled task: ``hy_recorder maint``).

Runs on the operator's machine, never on the recorder host. Each run:

1. ``pull``: mirror, verify and acknowledge the sealed segments (the durable copy of the data lives here);
2. a read-only probe (``hostprobe.py``) is piped to the host over ssh: service, disk, retention, ledger,
   clock, memory, journal and co-tenant health;
3. local checks (free space under the lake); once a week the whole lake is verified and a report is saved;
4. the findings are written to ``latest.txt`` and ``status.json`` under ``logs/``; the exit code is 0 (ok or
   info only), 1 (WARN) or 2 (CRIT), which Task Scheduler shows as the task's last result.

Nothing here deletes data: the only files it ever removes are its own logs older than ``log_keep_days``.
"""

from __future__ import annotations

import json
import os
import re
import shlex
import shutil
import subprocess
import time
from collections.abc import Callable
from dataclasses import dataclass, field, fields
from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import Any

from . import hostprobe
from .winproc import run_hidden

OK, INFO, WARN, CRIT = "OK", "INFO", "WARN", "CRIT"
_RANK = {OK: 0, INFO: 0, WARN: 1, CRIT: 2}
GB, MB = 1 << 30, 1 << 20
_SSH_CONNECTION_FAILED = 255
_IPV4 = re.compile(r"\b\d{1,3}(?:\.\d{1,3}){3}\b")
LOCK_STALE_S = 3 * 3600.0
_GC_PATTERNS = ("maint-*.log", "verify-*.txt", "report-*.txt")

# Every ledger kind the host probe counts over the last 24 h (``hostprobe.INTEREST``) either raises a
# finding here or is declared routine below; a test pins that the two together are exactly INTEREST. A
# kind that is counted but judged by nobody is how a stall of all four connections (2026-10-05, 31 to
# 53 s of lost data per connection) once went unreported while the run said ``findings: none``.
EVENT_FINDINGS: tuple[tuple[str, str, str], ...] = (
    ("DISK_STOP", CRIT, "recording stopped to protect the disk"),
    ("BAN", CRIT, "HTTP 418 ban"),
    ("DISK_WARN", WARN, "host disk pressure"),
    ("RATE_LIMIT", WARN, "REST rate limit"),
    ("TASK_CRASH", WARN, "a recorder task crashed and was restarted"),
    ("OVERRUN", WARN, "queue overrun: records dropped"),
    ("STREAM_STALL", WARN, "a live stream went silent and its connection was recycled: a data gap"),
    ("WS_CONNECT_FAIL", WARN, "a connection attempt failed (it is retried)"),
    ("GAP_DETECTED", WARN, "the recorder itself saw a sequence gap"),
    ("SUBSCRIBED_NO_DATA", WARN, "a subscribed stream never delivered data"),
    ("BAD_FRAME", WARN, "a frame went to the fallback record"),
    ("LOOP_LAG", WARN, "the event loop stalled: timestamps of that moment are late"),
    ("REST_FROZEN", WARN, "REST traffic was frozen"),
    ("RETENTION_STEPDOWN", WARN, "the retention ladder stepped down under disk pressure"),
)
# the normal heartbeat of a healthy recorder; EVICTED_UNACKED is judged from the lake section (CRIT)
ROUTINE_EVENTS = frozenset(
    {"WS_OPEN", "WS_CLOSE", "PRUNED_ACKED", "PROC_START", "PROC_STOP", "EVICTED_UNACKED"}
)


@dataclass(frozen=True)
class Thresholds:
    host_disk_warn_pct: float = 70.0
    host_disk_crit_pct: float = 85.0
    unacked_warn_h: float = 36.0  # the oldest sealed segment nobody has pulled and acknowledged yet
    unacked_crit_h: float = 96.0
    local_free_warn_gb: float = 40.0
    local_free_crit_gb: float = 15.0
    anon_warn_mb: float = 300.0  # the unit's MemoryHigh is 400 MB
    journal_warn_mb: float = 250.0  # the drop-in caps it at 200 MB
    clock_unsynced_warn_s: float = 600.0
    clock_marker_stale_s: float = 3 * 3600.0
    rotation_hole_warn_s: float = (
        2.0  # a planned rotation should cost well under a second (it cost 5.3 s once)
    )
    no_success_crit_h: float = 48.0
    log_keep_days: int = 60


@dataclass(frozen=True)
class MaintConfig:
    host: str
    lake: Path
    logs: Path
    remote_root: str = "/var/lib/hy-recorder"
    ssh: str = "ssh"
    min_interval_h: float = 6.0  # a catch-up trigger right after a successful run does nothing
    verify_every_days: float = 7.0
    thresholds: Thresholds = field(default_factory=Thresholds)


def load_config(path: str | Path) -> MaintConfig:
    raw = json.loads(Path(path).read_text(encoding="utf-8"))
    th = raw.pop("thresholds", {})
    known = {f.name for f in fields(Thresholds)}
    unknown = sorted(set(th) - known)
    if unknown:
        raise ValueError("unknown thresholds: %s" % ", ".join(unknown))
    raw["lake"], raw["logs"] = Path(raw["lake"]), Path(raw["logs"])
    return MaintConfig(**raw, thresholds=Thresholds(**th))


@dataclass(frozen=True)
class Finding:
    level: str
    code: str
    msg: str


def redact(text: str) -> str:
    """ssh error text carries the host address; it must not end up in files that get pasted around."""
    return _IPV4.sub("<ip>", text)


def _parse_utc(text: str | None) -> datetime | None:
    try:
        return datetime.fromisoformat(text).astimezone(UTC) if text else None
    except ValueError:
        return None


def _rotation_gaps(probe: dict[str, Any]) -> list[float]:
    """WS_CLOSE -> next WS_OPEN seconds of the planned rotations of the running recorder (last 24 h)."""
    gaps = (probe.get("ledger") or {}).get("reconnect_gaps") or []
    return [
        g["gap_s"] for g in gaps if g.get("reason") == "rotation" and isinstance(g.get("gap_s"), (int, float))
    ]


def evaluate(
    *,
    pull: dict[str, Any] | None,
    pull_error: str | None,
    probe: dict[str, Any] | None,
    probe_error: str | None,
    verify: dict[str, Any] | None,
    local_free_bytes: int | None,
    state: dict[str, Any],
    cfg: MaintConfig,
    now: datetime,
) -> list[Finding]:
    """Pure: every judgement of a run in one place (the thresholds live in :class:`Thresholds`)."""
    th, out = cfg.thresholds, []

    def add(level: str, code: str, msg: str) -> None:
        out.append(Finding(level, code, msg))

    # -- pull ------------------------------------------------------------------------------------
    if pull_error is not None:
        n = int(state.get("consecutive_pull_failures", 0)) + 1
        add(WARN if n == 1 else CRIT, "PULL_FAILED", "pull failed (%d in a row): %s" % (n, pull_error))
    elif pull is not None:
        if pull["errors"]:
            add(
                CRIT,
                "PULL_ERRORS",
                "%d segment(s) failed to mirror: %s" % (len(pull["errors"]), pull["errors"][0]),
            )
        if pull["lost"]:
            add(
                CRIT,
                "PULL_LOST",
                "%d segment(s) reclaimed on the host before they were pulled" % len(pull["lost"]),
            )

    # -- host ------------------------------------------------------------------------------------
    if probe_error is not None:
        n = int(state.get("consecutive_probe_failures", 0)) + 1
        add(
            WARN if n == 1 else CRIT,
            "HOST_UNREACHABLE",
            "host probe failed (%d in a row): %s" % (n, probe_error),
        )
    elif probe is not None:
        if probe.get("errors"):
            add(INFO, "PROBE_PARTIAL", "sub-probes failed: %s" % "; ".join(probe["errors"]))
        svc = probe.get("service") or {}
        if svc.get("ActiveState") != "active":
            add(
                CRIT,
                "RECORDER_INACTIVE",
                "hy-recorder is %s/%s" % (svc.get("ActiveState"), svc.get("SubState")),
            )
        rebooted = state.get("boot_id") not in (None, probe.get("boot_id"))
        if rebooted:
            add(
                INFO,
                "HOST_REBOOTED",
                "boot id changed (%s -> %s)" % (state.get("boot_id"), probe.get("boot_id")),
            )
        elif state.get("nrestarts") is not None and (svc.get("NRestarts") or 0) > state["nrestarts"]:
            add(WARN, "RECORDER_RESTARTED", "restarts %s -> %s" % (state["nrestarts"], svc["NRestarts"]))
        used = (probe.get("disk") or {}).get("used_pct")
        if used is not None and used >= th.host_disk_crit_pct:
            add(CRIT, "HOST_DISK", "host disk %.1f %% used" % used)
        elif used is not None and used >= th.host_disk_warn_pct:
            add(WARN, "HOST_DISK", "host disk %.1f %% used" % used)
        lake = probe.get("lake") or {}
        if lake.get("evicted_unacked_24h"):
            add(
                CRIT,
                "EVICTED_UNACKED",
                "%d unacknowledged segment(s) evicted in 24 h: data lost" % lake["evicted_unacked_24h"],
            )
        if lake.get("dangling_live"):
            add(
                CRIT,
                "MANIFEST_DANGLING",
                "%d manifest segment(s) missing on the host" % lake["dangling_live"],
            )
        age = lake.get("oldest_unacked_age_h")
        if age is not None and age >= th.unacked_crit_h:
            add(CRIT, "UNACKED_BACKLOG", "oldest unacknowledged segment is %.1f h old" % age)
        elif age is not None and age >= th.unacked_warn_h:
            add(WARN, "UNACKED_BACKLOG", "oldest unacknowledged segment is %.1f h old" % age)
        ev = (probe.get("ledger") or {}).get("events_24h") or {}
        for kind, level, why in EVENT_FINDINGS:
            if ev.get(kind):
                add(level, kind, "%d x %s in 24 h (%s)" % (ev[kind], kind, why))
        rotation_gaps = _rotation_gaps(probe)
        if rotation_gaps and max(rotation_gaps) > th.rotation_hole_warn_s:
            add(
                WARN,
                "ROTATION_HOLE",
                "a planned rotation left a %.1f s hole (expected under %.1f s): the close timeout regressed?"
                % (max(rotation_gaps), th.rotation_hole_warn_s),
            )
        clk, last = probe.get("clock") or {}, (probe.get("ledger") or {}).get("last_clock_state") or {}
        if last.get("synced") is False and isinstance(last.get("t"), int):
            since = (now.timestamp() * 1e6 - last["t"]) / 1e6
            if since > th.clock_unsynced_warn_s:
                add(
                    WARN,
                    "CLOCK_UNSYNCED",
                    "clock unsynchronised for %.0f s (%s)" % (since, last.get("reason")),
                )
        if clk.get("marker_age_s") is not None and clk["marker_age_s"] > th.clock_marker_stale_s:
            add(WARN, "CLOCK_STALE", "no NTP exchange for %.1f h" % (clk["marker_age_s"] / 3600))
        mem = probe.get("memory") or {}
        if (mem.get("anon_bytes") or 0) > th.anon_warn_mb * MB:
            add(WARN, "MEMORY_HIGH", "anon memory %.0f MB" % (mem["anon_bytes"] / MB))
        if (mem.get("events") or {}).get("oom_kill"):
            add(CRIT, "OOM_KILL", "the recorder cgroup had %d OOM kill(s)" % mem["events"]["oom_kill"])
        jr = probe.get("journal") or {}
        if jr.get("persistent_dir") is False:
            add(
                WARN,
                "JOURNAL_VOLATILE",
                "the host journal is not persistent any more (/var/log/journal is gone)",
            )
        if (jr.get("disk_usage_bytes") or 0) > th.journal_warn_mb * MB:
            add(WARN, "JOURNAL_BIG", "journal uses %.0f MB" % (jr["disk_usage_bytes"] / MB))
        co = probe.get("co_tenants") or {}
        down = [k for k in ("x-ui", "fail2ban") if co.get(k) != "active"] + (
            ["xray"] if not co.get("xray_procs") else []
        )
        if down:
            add(WARN, "COTENANT_DOWN", "not running on the host: %s" % ", ".join(down))

    # -- verify and local ------------------------------------------------------------------------
    if verify is not None:
        if verify["ok"]:
            add(INFO, "VERIFY_OK", verify["result"])
        else:
            add(CRIT, "VERIFY_FAIL", verify["result"])
    if local_free_bytes is not None:
        free_gb = local_free_bytes / GB
        if free_gb < th.local_free_crit_gb:
            add(CRIT, "LOCAL_DISK_LOW", "%.1f GB free under the lake" % free_gb)
        elif free_gb < th.local_free_warn_gb:
            add(WARN, "LOCAL_DISK_LOW", "%.1f GB free under the lake" % free_gb)

    # -- staleness -------------------------------------------------------------------------------
    this_ok = pull_error is None and probe_error is None and not any(f.level == CRIT for f in out)
    last_ok = _parse_utc(state.get("last_success_utc"))
    if not this_ok and last_ok is not None and now - last_ok > timedelta(hours=th.no_success_crit_h):
        add(CRIT, "NO_RECENT_SUCCESS", "no successful run since %s" % last_ok.strftime("%Y-%m-%d %H:%MZ"))
    return out


def overall(findings: list[Finding]) -> str:
    worst = max((_RANK[f.level] for f in findings), default=0)
    return {0: OK, 1: WARN, 2: CRIT}[worst]


# -- the real collaborators (replaced by fakes in the tests) ---------------------------------------------


def do_pull(cfg: MaintConfig, log: Callable[[str], None]) -> dict[str, Any]:
    from .pull import SshTransport, pull

    res = pull(SshTransport(cfg.host, cfg.remote_root, ssh=cfg.ssh), cfg.lake, ack=True, log=log)
    return {
        "ok": res.ok,
        "fetched": len(res.fetched),
        "bytes": res.bytes_fetched,
        "already": len(res.already_had),
        "acked": len(res.acked),
        "errors": list(res.errors),
        "lost": list(res.evicted_unfetched),
    }


def do_probe(
    cfg: MaintConfig,
    run: Callable[..., subprocess.CompletedProcess[bytes]] = run_hidden,
    sleep: Callable[[float], None] = time.sleep,
) -> dict[str, Any]:
    """Pipe ``hostprobe.py`` to the host; ssh exit 255 (the connection itself failed) is retried."""
    payload = Path(hostprobe.__file__).read_bytes()
    argv = [
        cfg.ssh,
        *("-o", "BatchMode=yes", "-o", "ConnectTimeout=25", "-o", "ServerAliveInterval=15"),
        cfg.host,
        "nice -n 19 python3 - " + shlex.quote(cfg.remote_root),
    ]
    proc = run(argv, input=payload, capture_output=True, timeout=180)
    for delay in (5.0, 15.0):
        if proc.returncode != _SSH_CONNECTION_FAILED:
            break
        sleep(delay)
        proc = run(argv, input=payload, capture_output=True, timeout=180)
    if proc.returncode != 0:
        tail = proc.stderr.decode("utf-8", "replace").strip().splitlines()[-1:] or [""]
        raise RuntimeError("ssh/probe exit %d: %s" % (proc.returncode, redact(tail[0])[:160]))
    lines = proc.stdout.decode("utf-8", "replace").strip().splitlines()
    return json.loads(lines[-1])


def do_verify(cfg: MaintConfig) -> dict[str, Any]:
    from .verify import verify_lake

    rep = verify_lake(cfg.lake, check_hashes=True)
    text = rep.render_text()
    return {"ok": rep.exit_code == 0, "result": text.strip().splitlines()[-1], "text": text}


def do_report(cfg: MaintConfig) -> str:
    from .report import build_report, render_text

    return render_text(build_report(cfg.lake))


# -- one run --------------------------------------------------------------------------------------------


def _acquire_lock(path: Path, now_s: float) -> bool:
    try:
        fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    except FileExistsError:
        try:
            if now_s - path.stat().st_mtime < LOCK_STALE_S:
                return False
        except OSError:
            return False
        path.write_text("%d\n" % os.getpid())  # a crashed run left a stale lock behind
        return True
    with os.fdopen(fd, "w") as fh:
        fh.write("%d\n" % os.getpid())
    return True


def _render(level: str, now: datetime, facts: list[str], findings: list[Finding]) -> str:
    lines = ["HengYuan D0 maintenance  %s  level %s" % (now.strftime("%Y-%m-%dT%H:%M:%SZ"), level), *facts]
    if findings:
        lines.append("findings (%d):" % len(findings))
        lines += ["  [%s] %s: %s" % (f.level, f.code, f.msg) for f in findings]
    else:
        lines.append("findings: none")
    return "\n".join(lines) + "\n"


def _facts(
    pull: dict[str, Any] | None, probe: dict[str, Any] | None, verify: dict[str, Any] | None, free: int | None
) -> list[str]:
    facts = []
    if pull is not None:
        facts.append(
            "pull: fetched %d (%.1f MB), already had %d, acked %d, errors %d, lost %d"
            % (
                pull["fetched"],
                pull["bytes"] / 1e6,
                pull["already"],
                pull["acked"],
                len(pull["errors"]),
                len(pull["lost"]),
            )
        )
    if probe is not None:
        svc, disk, lake = probe.get("service") or {}, probe.get("disk") or {}, probe.get("lake") or {}
        facts.append(
            "host: recorder %s, restarts %s, up %.1f d, disk %s %% used, "
            "%s live segments (%s unacked, oldest %s h)"
            % (
                svc.get("ActiveState"),
                svc.get("NRestarts"),
                (probe.get("uptime_s") or 0) / 86400,
                disk.get("used_pct"),
                lake.get("live_segments"),
                lake.get("unacked_segments"),
                lake.get("oldest_unacked_age_h"),
            )
        )
        rotations = _rotation_gaps(probe)
        if rotations:
            facts.append(
                "rotations (running recorder, last 24 h): %d, hole %.2f-%.2f s"
                % (len(rotations), min(rotations), max(rotations))
            )
    if verify is not None:
        facts.append("verify: " + verify["result"])
    if free is not None:
        facts.append("local free space under the lake: %.1f GB" % (free / GB))
    return facts


def run_maintenance(
    cfg: MaintConfig,
    *,
    force: bool = False,
    now: datetime | None = None,
    pull_fn: Callable[[MaintConfig, Callable[[str], None]], dict[str, Any]] = do_pull,
    probe_fn: Callable[[MaintConfig], dict[str, Any]] = do_probe,
    verify_fn: Callable[[MaintConfig], dict[str, Any]] = do_verify,
    report_fn: Callable[[MaintConfig], str] = do_report,
    free_fn: Callable[[Path], int] = lambda p: shutil.disk_usage(p).free,
    log: Callable[[str], None] = lambda m: print(m, flush=True),
) -> int:
    now = now or datetime.now(UTC)
    logs = cfg.logs
    logs.mkdir(parents=True, exist_ok=True)
    state_path = logs / "state.json"
    try:
        state = json.loads(state_path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        state = {}
    last_ok = _parse_utc(state.get("last_success_utc"))
    if not force and last_ok is not None and now - last_ok < timedelta(hours=cfg.min_interval_h):
        log(
            "skipped: the last successful run was %s (min interval %.1f h)"
            % (last_ok.isoformat(), cfg.min_interval_h)
        )
        return 0
    if not _acquire_lock(logs / "maint.lock", now.timestamp()):
        log("skipped: another maintenance run holds %s" % (logs / "maint.lock"))
        return 0
    stamp = now.strftime("%Y%m%d-%H%M%S")
    try:
        detail: list[str] = []
        pull_res, pull_error = None, None
        try:
            pull_res = pull_fn(cfg, lambda m: detail.append("pull: " + redact(m)))
        except Exception as exc:  # noqa: BLE001 - any pull failure is a finding, not a crash of the maintenance run
            pull_error = redact("%s: %s" % (type(exc).__name__, str(exc)))[:200]
        probe, probe_error = None, None
        try:
            probe = probe_fn(cfg)
        except Exception as exc:  # noqa: BLE001
            probe_error = redact("%s: %s" % (type(exc).__name__, str(exc)))[:200]
        last_verify = _parse_utc(state.get("last_verify_utc"))
        verify = None
        if last_verify is None or now - last_verify >= timedelta(days=cfg.verify_every_days):
            try:
                verify = verify_fn(cfg)
                (logs / ("verify-%s.txt" % stamp)).write_text(verify["text"], encoding="utf-8")
                (logs / ("report-%s.txt" % stamp)).write_text(report_fn(cfg), encoding="utf-8")
            except Exception as exc:  # noqa: BLE001
                verify = {
                    "ok": False,
                    "result": "verify could not run: %s: %s" % (type(exc).__name__, str(exc)[:120]),
                    "text": "",
                }
        try:
            free = free_fn(cfg.lake)
        except OSError:
            free = None
        findings = evaluate(
            pull=pull_res,
            pull_error=pull_error,
            probe=probe,
            probe_error=probe_error,
            verify=verify,
            local_free_bytes=free,
            state=state,
            cfg=cfg,
            now=now,
        )
        level = overall(findings)
        text = _render(level, now, _facts(pull_res, probe, verify, free), findings)
        (logs / "latest.txt").write_text(text, encoding="utf-8")
        (logs / ("maint-%s.log" % stamp)).write_text(
            text
            + "\n"
            + "\n".join(detail)
            + "\n\nprobe: "
            + json.dumps(probe, indent=1, sort_keys=True)
            + "\n",
            encoding="utf-8",
        )
        status = {
            "level": level,
            "generated_utc": now.strftime("%Y-%m-%dT%H:%M:%SZ"),
            "findings": [{"level": f.level, "code": f.code, "msg": f.msg} for f in findings],
            "pull": pull_res,
            "verify": None if verify is None else {"ok": verify["ok"], "result": verify["result"]},
            "local_free_bytes": free,
        }
        (logs / "status.json").write_text(json.dumps(status, indent=1, sort_keys=True), encoding="utf-8")
        ok_run = pull_error is None and probe_error is None and level != CRIT
        svc = (probe or {}).get("service") or {}
        new_state = dict(state)
        new_state.update(
            last_run_utc=now.isoformat(),
            consecutive_pull_failures=0
            if pull_error is None
            else int(state.get("consecutive_pull_failures", 0)) + 1,
            consecutive_probe_failures=0
            if probe_error is None
            else int(state.get("consecutive_probe_failures", 0)) + 1,
        )
        if probe is not None:
            new_state.update(boot_id=probe.get("boot_id"), nrestarts=svc.get("NRestarts"))
        if ok_run:
            new_state["last_success_utc"] = now.isoformat()
        if verify is not None and verify["ok"]:
            new_state["last_verify_utc"] = now.isoformat()
        state_path.write_text(json.dumps(new_state, indent=1, sort_keys=True), encoding="utf-8")
        _collect_garbage(logs, now, cfg.thresholds.log_keep_days)
        log(text.rstrip())
        return _RANK[level]
    finally:
        try:
            (logs / "maint.lock").unlink()
        except OSError:
            pass


def _collect_garbage(logs: Path, now: datetime, keep_days: int) -> None:
    cutoff = now.timestamp() - keep_days * 86400
    for pattern in _GC_PATTERNS:
        for fp in logs.glob(pattern):
            try:
                if fp.stat().st_mtime < cutoff:
                    fp.unlink()
            except OSError:
                pass
