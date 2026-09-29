"""Structural checks of the files that end up on the recording host."""

from __future__ import annotations

import re
import shutil
import subprocess
from pathlib import Path

import pytest

from hy_recorder.config import load_config

ROOT = Path(__file__).resolve().parents[1]
DEPLOY = ROOT / "deploy"


def parse_unit(text: str) -> dict[str, dict[str, list[str]]]:
    sections: dict[str, dict[str, list[str]]] = {}
    current = None
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("[") and line.endswith("]"):
            current = sections.setdefault(line[1:-1], {})
        elif current is not None and "=" in line:
            key, _, value = line.partition("=")
            current.setdefault(key.strip(), []).append(value.strip())
    return sections


@pytest.fixture(scope="module")
def unit():
    return parse_unit((DEPLOY / "hy-recorder.service").read_text(encoding="utf-8"))


def test_unit_runs_the_bundle_from_pythonpath_as_an_unprivileged_user(unit):
    svc = unit["Service"]
    assert svc["Type"] == ["simple"] and svc["User"] == ["hyrec"] and svc["Group"] == ["hyrec"]
    env = " ".join(svc["Environment"])
    assert 'PYTHONPATH=/opt/hy-recorder/releases/current"' in env
    assert "PYTHONDONTWRITEBYTECODE=1" in env and "TZ=UTC" in env
    assert svc["ExecStart"] == ["/usr/bin/python3 -m hy_recorder run --config /etc/hy-recorder/recorder.toml"]
    assert svc["ExecStartPre"] == ['/usr/bin/python3 -c "import websockets, zstandard, hy_recorder"']
    assert "EnvironmentFile" not in svc, "the recorder reads no environment file (and holds no credentials)"


def test_unit_restart_policy_survives_forever_without_a_storm(unit):
    svc = unit["Service"]
    assert (
        svc["Restart"] == ["always"] and svc["RestartSec"] == ["10"] and svc["StartLimitIntervalSec"] == ["0"]
    )
    assert svc["TimeoutStopSec"] == ["45"] and svc["KillSignal"] == ["SIGTERM"]
    assert "WatchdogSec" not in svc and "NotifyAccess" not in svc


def test_unit_resource_limits_yield_to_the_co_tenant_without_a_hard_cpu_cap(unit):
    svc = unit["Service"]
    assert svc["CPUWeight"] == ["20"] and svc["Nice"] == ["10"] and svc["IOWeight"] == ["20"]
    assert svc["OOMScoreAdjust"] == ["500"] and svc["MemoryHigh"] == ["400M"] and svc["MemoryMax"] == ["600M"]
    assert svc["TasksMax"] == ["64"]
    assert "CPUQuota" not in svc, "a hard quota adds up to 40 ms of scheduling delay to receive timestamps"
    assert "SocketBindDeny" not in svc, (
        "unproven effect on resolver sockets; 'no listener' is proven by ss -ltnup"
    )


def test_unit_sandbox_and_state_directory(unit):
    svc = unit["Service"]
    for key in ("NoNewPrivileges", "ProtectSystem", "ProtectHome", "PrivateTmp", "PrivateDevices"):
        assert key in svc, key
    assert svc["ProtectSystem"] == ["strict"] and svc["NoNewPrivileges"] == ["yes"]
    assert svc["RestrictAddressFamilies"] == ["AF_UNIX AF_INET AF_INET6"]
    assert svc["StateDirectory"] == ["hy-recorder"] and svc["WorkingDirectory"] == ["/var/lib/hy-recorder"]
    assert unit["Install"]["WantedBy"] == ["multi-user.target"]


def test_example_config_loads_and_derives_the_expected_connections():
    cfg = load_config(DEPLOY / "recorder.toml")
    assert str(cfg.root).replace("\\", "/") == "/var/lib/hy-recorder"
    assert cfg.symbols == ("BTCUSDT",) and len(cfg.connections) == 4
    assert cfg.retain_hours == 48 and cfg.disk_warn_gb == 5 and cfg.disk_floor_gb == 2


SCRIPTS = sorted(DEPLOY.glob("*.sh"))


def test_the_expected_scripts_exist():
    assert {p.name for p in SCRIPTS} == {"install.sh", "uninstall.sh"}


@pytest.mark.parametrize("script", SCRIPTS, ids=lambda p: p.name)
def test_shell_scripts_are_strict_lf_and_parse(script):
    raw = script.read_bytes()
    text = raw.decode("utf-8")
    assert text.startswith("#!/usr/bin/env bash\n") and "set -euo pipefail" in text
    if b"\r\n" in raw:
        pytest.skip("working copy has CRLF (git normalises to LF: see recorder/.gitattributes)")
    bash = shutil.which("bash")
    if bash is None:
        pytest.skip("no bash on PATH")
    proc = subprocess.run([bash, "-n", str(script)], capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr


@pytest.mark.parametrize("script", SCRIPTS, ids=lambda p: p.name)
def test_scripts_touch_nothing_beyond_the_recorder_s_own_footprint(script):
    text = script.read_text(encoding="utf-8")
    code = "\n".join(line for line in text.splitlines() if not line.lstrip().startswith("#"))
    forbidden = (
        r"\b(iptables|ip6tables|nft|ufw|firewall-cmd|resolvectl|resolv\.conf|reboot|shutdown|apt(-get)?|"
        r"curl|wget|ssh|x-ui|xray|fail2ban|acme)\b"
    )
    assert not re.search(forbidden, code), re.search(forbidden, code).group(0)
    for path in re.findall(r"(/(?:etc|opt|var|usr)/[A-Za-z0-9_./\-]*)", code):
        allowed = (
            "/opt/hy-recorder",
            "/etc/hy-recorder",
            "/var/lib/hy-recorder",
            "/etc/systemd/system/hy-recorder.service",
            "/usr/sbin/nologin",
            "/usr/bin/env",
        )
        assert path.startswith(allowed) or path.rstrip("/") in ("/etc/systemd/system",), path
