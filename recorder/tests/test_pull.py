from __future__ import annotations

import io
import json
import os
from pathlib import Path

import pytest
from lake import SPOT_TRADE, LakeBuilder

from hy_recorder.manifest import Manifest
from hy_recorder.pull import LocalTransport, PullError, SshTransport, pull
from hy_recorder.verify import verify_lake


def trade(t):
    return json.dumps({"stream": "btcusdt@trade", "data": {"e": "trade", "t": t}}).encode()


def make_source(
    root: Path,
    hours: int = 3,
    per_hour: int = 20,
    *,
    builder: LakeBuilder | None = None,
    start_t: int = 1,
    stop: bool = True,
) -> LakeBuilder:
    """A lake with one sealed segment per (fake) hour of trades."""
    b = builder or LakeBuilder(root)
    gen = b.open_conn("spot_trade", [SPOT_TRADE])
    t = start_t
    for _ in range(hours):
        for _ in range(per_hour):
            b.record("spot", "trade", SPOT_TRADE, trade(t), gen)
            t += 1
        b.clock.advance(3600)
    if stop:
        b.stop()
    return b


def seg_names(root: Path) -> list[str]:
    return sorted(Manifest(root).live_segments())


def shas(root: Path) -> set[str]:
    return {ev["sha256"] for ev in Manifest(root).live_segments().values()}


def test_pull_mirrors_verifies_and_acknowledges(tmp_path):
    src, dst = tmp_path / "src", tmp_path / "dst"
    make_source(src)
    names = seg_names(src)
    assert len(names) == 3
    res = pull(LocalTransport(src), dst)
    assert res.ok and sorted(res.fetched) == names and res.already_had == [] and res.bytes_fetched > 0
    assert sorted(res.acked) == sorted(shas(src))
    assert {p.name for p in (src / "acks").iterdir()} == shas(src)
    for name in names:
        assert (dst / name).read_bytes() == (src / name).read_bytes()
    report = verify_lake(dst)
    assert report.exit_code == 0, report.render_text()
    assert (
        dst / "status.remote.json"
    ).exists() is False  # the fixture source has no status.json: informational only


def test_a_second_pull_moves_nothing_and_a_later_pull_only_the_new_segments(tmp_path):
    src, dst = tmp_path / "src", tmp_path / "dst"
    make_source(src, hours=2)
    first = pull(LocalTransport(src), dst)
    again = pull(LocalTransport(src), dst)
    assert again.ok and again.fetched == [] and len(again.already_had) == 2 and again.acked == []
    assert first.bytes_fetched > 0 and again.bytes_fetched == 0
    # the recorder restarts: run 2 writes more data into the same root
    run2 = LakeBuilder(src)
    run2.clock.advance(2 * 86400)  # a restart happens later in wall time than the run it follows
    make_source(src, hours=1, start_t=1000, builder=run2)
    third = pull(LocalTransport(src), dst)
    assert third.ok and len(third.fetched) == 1 and len(third.already_had) == 2 and len(third.acked) == 1
    assert verify_lake(dst).exit_code == 0


class Flaky(LocalTransport):
    def __init__(self, root, mode):
        super().__init__(root)
        self.mode, self.calls = mode, 0

    def stream_file(self, rel):
        self.calls += 1
        chunks = list(super().stream_file(rel))
        if self.calls == 2 and rel.startswith("raw/"):  # the second segment gets damaged in transit
            data = b"".join(chunks)
            if self.mode == "flip":
                data = data[:10] + bytes([data[10] ^ 0xFF]) + data[11:]
            elif self.mode == "truncate":
                data = data[: len(data) // 2]
            chunks = [data]
        yield from chunks


@pytest.mark.parametrize("mode", ["flip", "truncate"])
def test_a_damaged_transfer_is_refused_removed_and_never_acknowledged(tmp_path, mode):
    src, dst = tmp_path / "src", tmp_path / "dst"
    make_source(src)
    names = seg_names(src)
    res = pull(Flaky(src, mode), dst)
    assert not res.ok and len(res.errors) == 1 and "verification failed" in res.errors[0]
    bad = names[1]
    assert bad in res.errors[0]
    assert not (dst / bad).exists() and not list(dst.rglob("*.pulltmp"))
    assert len(res.fetched) == 2 and bad not in res.fetched
    bad_sha = Manifest(src).live_segments()[bad]["sha256"]
    assert bad_sha not in {p.name for p in (src / "acks").iterdir()} and len(res.acked) == 2
    # the local archive is honest about being incomplete: its manifest has no seal for the missing segment
    assert bad not in Manifest(dst).live_segments()
    report = verify_lake(dst)
    assert report.exit_code == 1 and "SEQ_HOLE" in {i.code for i in report.issues}
    # the next pull (transport healthy again) repairs it and only then acknowledges it
    fixed = pull(LocalTransport(src), dst)
    assert fixed.ok and fixed.fetched == [bad] and fixed.acked == [bad_sha]
    assert verify_lake(dst).exit_code == 0


def test_a_torn_last_ledger_line_is_dropped(tmp_path):
    src, dst = tmp_path / "src", tmp_path / "dst"
    make_source(src, hours=1)
    ledger = next((src / "ledger").glob("*.jsonl"))
    with open(ledger, "ab") as fh:
        fh.write(b'{"run":1,"q":99999,"k":"HALF_WRIT')
    res = pull(LocalTransport(src), dst)
    assert res.ok
    text = (dst / "ledger" / ledger.name).read_bytes()
    assert text.endswith(b"\n") and b"HALF_WRIT" not in text
    assert verify_lake(dst).exit_code == 0


def test_a_segment_the_host_reclaimed_before_it_was_pulled_is_reported_loudly(tmp_path):
    src, dst = tmp_path / "src", tmp_path / "dst"
    make_source(src)
    names = seg_names(src)
    ev = Manifest(src).live_segments()[names[0]]
    os.remove(src / names[0])
    Manifest(src).append(
        {
            "ev": "prune",
            "name": names[0],
            "sha256": ev["sha256"],
            "reason": "evicted_unacked",
            "acked": False,
            "first_t": ev["first"]["t"],
            "last_t": ev["last"]["t"],
            "bytes": ev["bytes"],
            "level_h": 6.0,
            "first": ev["first"],
            "last": ev["last"],
            "wall_us": ev["wall_us"] + 10,
        }
    )
    res = pull(LocalTransport(src), dst)
    assert not res.ok and res.evicted_unfetched == [names[0]] and not res.errors
    assert len(res.fetched) == 2
    # the prune event stays in the local manifest: it explains the resulting sequence hole
    report = verify_lake(dst)
    assert report.exit_code == 0, report.render_text()
    assert "SEQ_HOLE" not in {i.code for i in report.issues}


def test_the_archive_keeps_what_the_host_later_reclaims(tmp_path):
    src, dst = tmp_path / "src", tmp_path / "dst"
    make_source(src)
    names = seg_names(src)
    assert pull(LocalTransport(src), dst).ok
    ev = Manifest(src).live_segments()[names[0]]
    os.remove(src / names[0])
    Manifest(src).append(
        {
            "ev": "prune",
            "name": names[0],
            "sha256": ev["sha256"],
            "reason": "acked_expired",
            "acked": True,
            "first_t": ev["first"]["t"],
            "last_t": ev["last"]["t"],
            "bytes": ev["bytes"],
            "level_h": 48.0,
            "first": ev["first"],
            "last": ev["last"],
            "wall_us": ev["wall_us"] + 10,
        }
    )
    res = pull(LocalTransport(src), dst)
    assert res.ok and res.evicted_unfetched == [] and len(res.already_had) == 2
    assert (dst / names[0]).exists(), "the local archive is never pruned by the host's retention"
    assert names[0] in Manifest(dst).live_segments()
    assert not any(e.get("ev") == "prune" for e in Manifest(dst).read_all())
    assert verify_lake(dst).exit_code == 0


def test_dry_run_reports_but_changes_nothing(tmp_path):
    src, dst = tmp_path / "src", tmp_path / "dst"
    make_source(src)
    res = pull(LocalTransport(src), dst, dry_run=True)
    assert len(res.fetched) == 3 and res.acked == []
    assert not dst.exists() or not any(dst.rglob("*.zst"))
    assert not (src / "acks").exists() or not list((src / "acks").iterdir())


def test_no_ack_option_leaves_the_host_unmodified(tmp_path):
    src, dst = tmp_path / "src", tmp_path / "dst"
    make_source(src)
    res = pull(LocalTransport(src), dst, ack=False)
    assert res.ok and res.acked == []
    assert not (src / "acks").exists() or not list((src / "acks").iterdir())


def test_unexpected_names_in_a_manifest_are_never_used_as_paths(tmp_path):
    src, dst = tmp_path / "src", tmp_path / "dst"
    make_source(src, hours=1)
    Manifest(src).append(
        {
            "ev": "seal",
            "name": "raw/../../etc/passwd",
            "bytes": 1,
            "sha256": "0" * 64,
            "wall_us": 1_790_000_000_000_000,
        }
    )
    res = pull(LocalTransport(src), dst)
    assert any("unexpected segment name" in e for e in res.errors)
    assert not (tmp_path / "etc").exists() and len(res.fetched) == 1


def test_transport_refuses_paths_outside_its_fixed_vocabulary(tmp_path):
    t = LocalTransport(tmp_path)
    for bad in (
        "../secret",
        "raw/../x",
        "raw/spot/trade/20260929/../../../../x.jsonl.zst",
        "ledger/../../x",
        "/etc/passwd",
    ):
        with pytest.raises(PullError):
            t.read_file(bad)


# -- ssh transport ---------------------------------------------------------------------------------
class Proc:
    def __init__(self, out=b"", err=b"", code=0):
        self.stdout, self.stderr, self.returncode, self._code = io.BytesIO(out), io.BytesIO(err), code, code

    def wait(self):
        return self._code


class Recorded:
    def __init__(self):
        self.argvs: list[list[str]] = []
        self.responses: list = []

    def run(self, argv, **kw):
        self.argvs.append(argv)
        out, err, code = self.responses.pop(0) if self.responses else (b"", b"", 0)

        class Done:
            returncode, stdout, stderr = code, out, err

        return Done()

    def popen(self, argv, **kw):
        self.argvs.append(argv)
        out, err, code = self.responses.pop(0) if self.responses else (b"", b"", 0)
        return Proc(out, err, code)


def test_ssh_commands_are_fixed_quoted_and_use_the_operator_s_own_ssh(tmp_path):
    rec = Recorded()
    t = SshTransport("tokyo-vps-8t", "/var/lib/hy-recorder/", run=rec.run, popen=rec.popen)
    rec.responses = [
        (b'{"ev":"seal"}\n', b"", 0),
        (b"20260929.jsonl\n20260930.jsonl\n", b"", 0),
        (b"", b"", 0),
    ]
    assert t.read_file("manifest/20260929.jsonl") == b'{"ev":"seal"}\n'
    assert t.list_dir("manifest") == ["20260929.jsonl", "20260930.jsonl"]
    t.write_acks(["a" * 64, "b" * 64])
    first, second, third = rec.argvs
    assert (
        first[:5] == ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=15"] and first[5] == "tokyo-vps-8t"
    )
    assert first[6] == "cat -- /var/lib/hy-recorder/manifest/20260929.jsonl"
    assert second[6].startswith("ls -1 -- /var/lib/hy-recorder/manifest")
    assert third[
        6
    ] == "mkdir -p /var/lib/hy-recorder/acks && cd /var/lib/hy-recorder/acks && touch -- %s %s" % (
        "a" * 64,
        "b" * 64,
    )
    with pytest.raises(PullError):
        t.write_acks(["not-a-sha; rm -rf /"])
    with pytest.raises(PullError):
        t.read_file("manifest/../../etc/shadow")
    assert len(rec.argvs) == 3, "a refused request must not reach ssh"


def test_ssh_stream_yields_chunks_and_surfaces_a_failed_remote_command():
    rec = Recorded()
    t = SshTransport("h", "/r", run=rec.run, popen=rec.popen)
    rec.responses = [(b"x" * 3_000_000, b"", 0)]
    assert sum(len(c) for c in t.stream_file("raw/spot/trade/20260929/10-000001.jsonl.zst")) == 3_000_000
    rec.responses = [(b"partial", b"cat: nope: Permission denied", 1)]
    with pytest.raises(PullError, match="Permission denied"):
        list(t.stream_file("raw/spot/trade/20260929/10-000001.jsonl.zst"))
    rec.responses = [(b"", b"boom", 255)]
    with pytest.raises(PullError):
        t.read_file("status.json")


def test_unchanged_local_segments_are_not_hashed_again_but_a_changed_one_is_refetched(tmp_path, monkeypatch):
    import hy_recorder.pull as pull_mod

    src, dst = tmp_path / "src", tmp_path / "dst"
    make_source(src)
    names = seg_names(src)
    assert pull(LocalTransport(src), dst).ok
    assert (dst / ".pull-cache.json").exists()

    def no_hashing(path):
        raise AssertionError("hashed %s although size and mtime are unchanged" % path)

    monkeypatch.setattr(pull_mod, "_sha256_file", no_hashing)
    again = pull(LocalTransport(src), dst)
    assert again.ok and again.fetched == [] and len(again.already_had) == 3
    monkeypatch.undo()
    # a local file whose size changed can never pass for the manifest's copy
    (dst / names[1]).write_bytes((dst / names[1]).read_bytes()[:-3])
    repaired = pull(LocalTransport(src), dst)
    assert repaired.ok and repaired.fetched == [names[1]]
    assert (dst / names[1]).read_bytes() == (src / names[1]).read_bytes()
