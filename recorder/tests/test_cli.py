from __future__ import annotations

import json

import pytest
from lake import SPOT_TRADE, LakeBuilder

import hy_recorder.pull as pull_mod
from hy_recorder.cli import main


def trade(t):
    return json.dumps({"stream": "btcusdt@trade", "data": {"e": "trade", "t": t}}).encode()


def small_lake(root):
    b = LakeBuilder(root)
    gen = b.open_conn("spot_trade", [SPOT_TRADE])
    for i in range(1, 30):
        b.record("spot", "trade", SPOT_TRADE, trade(i), gen)
    b.stop()
    return b


def test_config_check_prints_the_derived_connections(tmp_path, capsys):
    cfg = tmp_path / "recorder.toml"
    cfg.write_text(
        '[recorder]\nroot = "%s"\nsymbols = ["BTCUSDT"]\n' % str(tmp_path / "data").replace("\\", "/")
    )
    assert main(["config-check", "--config", str(cfg)]) == 0
    out = capsys.readouterr().out
    assert "wss://fstream.binance.com/public/stream?streams=btcusdt@depth@100ms" in out
    assert (
        "wss://fstream.binance.com/market/stream?streams=btcusdt@aggTrade/btcusdt@markPrice@1s/!forceOrder@arr"
        in out
    )
    assert out.count("wss://") == 4


def test_a_bad_config_is_a_clean_error_not_a_traceback(tmp_path, capsys):
    cfg = tmp_path / "recorder.toml"
    cfg.write_text(
        '[recorder]\nroot = "%s"\nurl = "wss://evil.example/ws"\n' % str(tmp_path).replace("\\", "/")
    )
    assert main(["config-check", "--config", str(cfg)]) == 2
    assert "unknown key" in capsys.readouterr().err


def test_verify_exit_code_follows_the_report(tmp_path, capsys):
    small_lake(tmp_path)
    assert main(["verify", "--root", str(tmp_path)]) == 0
    assert "RESULT: PASS" in capsys.readouterr().out
    assert main(["verify", "--root", str(tmp_path), "--json"]) == 0
    report = json.loads(capsys.readouterr().out)
    assert report["exit_code"] == 0 if "exit_code" in report else True
    seg = next((tmp_path / "raw").rglob("*.jsonl.zst"))
    data = bytearray(seg.read_bytes())
    data[len(data) // 2] ^= 0xFF
    seg.write_bytes(bytes(data))
    assert main(["verify", "--root", str(tmp_path)]) == 1
    assert "FAIL" in capsys.readouterr().out
    # --no-hash still catches a size change
    seg.write_bytes(bytes(data)[:-5])
    assert main(["verify", "--root", str(tmp_path), "--no-hash"]) == 1


def test_status_without_a_status_file_is_a_clear_message(tmp_path, capsys):
    assert main(["status", "--root", str(tmp_path)]) == 2
    assert "no status.json" in capsys.readouterr().err


def test_status_prints_the_file(tmp_path, capsys):
    (tmp_path / "status.json").write_text(json.dumps({"run": 3, "disk": {"free_bytes": 1}}))
    assert main(["status", "--root", str(tmp_path)]) == 0
    assert json.loads(capsys.readouterr().out)["run"] == 3


def test_pull_maps_the_result_to_an_exit_code(tmp_path, monkeypatch, capsys):
    seen = {}

    class FakeTransport:
        def __init__(self, host, remote_root, ssh="ssh"):
            seen.update(host=host, remote_root=remote_root, ssh=ssh)

    def fake_pull(transport, dest, ack, dry_run, log):
        seen.update(dest=str(dest), ack=ack, dry_run=dry_run)
        return pull_mod.PullResult(fetched=["a"], errors=seen.get("errors", []), bytes_fetched=5)

    monkeypatch.setattr(pull_mod, "SshTransport", FakeTransport)
    monkeypatch.setattr(pull_mod, "pull", fake_pull)
    assert main(["pull", "--host", "tokyo-vps-8t", "--dest", str(tmp_path / "d"), "--no-ack"]) == 0
    assert (
        seen["host"] == "tokyo-vps-8t"
        and seen["remote_root"] == "/var/lib/hy-recorder"
        and seen["ack"] is False
    )
    seen["errors"] = ["x: verification failed"]
    assert main(["pull", "--host", "h", "--dest", str(tmp_path / "d")]) == 1
    assert "ERROR: x: verification failed" in capsys.readouterr().err


def test_unknown_subcommand_is_rejected():
    with pytest.raises(SystemExit):
        main(["record-everything"])
