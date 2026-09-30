"""Offline verification of a lake: integrity, ordering, per-stream continuity, ledger cross-check.

``verify_lake`` never trusts the recorder's own ledger as evidence that data is fine: every
continuity fact is re-derived from the raw records. The ledger is used only to *explain* a gap
that the raw data already proves (a gap with no ledger explanation is a FAIL: "the ledger says
nothing happened" is exactly the false-safety this tool exists to catch).

Exit code: 0 = no FAIL, 1 = at least one FAIL, 2 = the lake could not be read.
"""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from ..envelope import EnvelopeError, Record, decode_record
from ..fsutil import sha256_file
from ..ledger import Ledger
from ..manifest import Manifest
from .reader import SegmentReader
from .streams import (
    FAIL,
    INFO,
    WARN,
    AggTradeVerifier,
    DepthVerifier,
    ForceOrderVerifier,
    Issue,
    MarkPriceVerifier,
    TradeVerifier,
    _Base,
)

CRASH_TAIL_US = 10_000_000  # holes this close to the end of an unclean run are the lost fsync window
MAX_LISTED = 200


@dataclass
class Report:
    root: str
    issues: list[Issue] = field(default_factory=list)
    streams: dict[str, dict[str, Any]] = field(default_factory=dict)
    segments: dict[str, Any] = field(default_factory=dict)
    runs: dict[int, dict[str, Any]] = field(default_factory=dict)

    def count(self, severity: str) -> int:
        return sum(1 for i in self.issues if i.severity == severity)

    @property
    def fail_count(self) -> int:
        return self.count(FAIL)

    @property
    def exit_code(self) -> int:
        return 1 if self.fail_count else 0

    def by_code(self) -> dict[str, dict[str, int]]:
        out: dict[str, dict[str, int]] = {}
        for i in self.issues:
            out.setdefault(i.code, {}).setdefault(i.severity, 0)
            out[i.code][i.severity] += 1
        return out

    def to_dict(self) -> dict[str, Any]:
        order = {FAIL: 0, WARN: 1, INFO: 2}
        listed = sorted(self.issues, key=lambda i: order[i.severity])[:MAX_LISTED]
        return {
            "root": self.root,
            "ok": self.fail_count == 0,
            "fail": self.fail_count,
            "warn": self.count(WARN),
            "info": self.count(INFO),
            "issue_counts": self.by_code(),
            "issues_listed": [i.as_dict() for i in listed],
            "streams": self.streams,
            "segments": self.segments,
            "runs": {str(k): v for k, v in sorted(self.runs.items())},
        }

    def render_text(self) -> str:
        lines = [
            "lake: %s" % self.root,
            "segments: %d sealed (%d bytes)"
            % (self.segments.get("sealed", 0), self.segments.get("bytes", 0)),
        ]
        for name, s in sorted(self.streams.items()):
            extra = ""
            if "verified_seconds" in s:
                extra = " verified %.1fs of %.1fs, gaps=%d" % (
                    s["verified_seconds"],
                    s["span_seconds"],
                    s["counters"].get("gaps", 0),
                )
            lines.append(
                "  %-38s records=%-9d gens=%d largest_gap=%.1fs%s"
                % (name, s["records"], s["generations"], s["largest_gap_s"], extra)
            )
        for code, sev in sorted(self.by_code().items()):
            lines.append("  %-30s %s" % (code, ", ".join("%s=%d" % kv for kv in sorted(sev.items()))))
        lines.append(
            "RESULT: %s (FAIL=%d WARN=%d INFO=%d)"
            % ("PASS" if self.exit_code == 0 else "FAIL", self.fail_count, self.count(WARN), self.count(INFO))
        )
        return "\n".join(lines)


class _DepthDriver:
    """Feeds a depth verifier and injects snapshot records in ``(run, seq)`` order."""

    def __init__(self, verifier: DepthVerifier, snapshots: list[Record]) -> None:
        self.v = verifier
        self._snaps = snapshots
        self._ptr = 0

    def feed(self, rec: Record) -> None:
        while self._ptr < len(self._snaps) and self._snaps[self._ptr].order < rec.order:
            self.v.feed_snapshot(self._snaps[self._ptr])
            self._ptr += 1
        self.v.feed(rec)

    def finish(self) -> None:
        while self._ptr < len(self._snaps):
            self.v.feed_snapshot(self._snaps[self._ptr])
            self._ptr += 1
        self.v.finish()


def _make_verifier(stream: str) -> _Base | None:
    venue, _, name = stream.partition(":")
    if "@depth" in name:
        return DepthVerifier(stream, venue)
    if name.endswith("@trade"):
        return TradeVerifier(stream)
    if name.endswith("@aggTrade"):
        return AggTradeVerifier(stream)
    if "@markPrice" in name:
        return MarkPriceVerifier(stream)
    if name.startswith("!forceOrder"):
        return ForceOrderVerifier(stream)
    return None


def _mark(bits: dict[int, bytearray], run: int, q: int) -> None:
    b = bits.setdefault(run, bytearray())
    if q >= len(b):
        b.extend(bytearray(max(q + 1 - len(b), len(b))))
    b[q] = 1


def _zero_runs(bits: bytearray, q_min: int, q_max: int) -> list[tuple[int, int]]:
    holes: list[tuple[int, int]] = []
    pos = q_min
    while pos <= q_max:
        z = bits.find(0, pos, q_max + 1)
        if z < 0:
            break
        o = bits.find(1, z, q_max + 1)
        end = (o - 1) if o >= 0 else q_max
        holes.append((z, end))
        pos = end + 1
    return holes


def _subtract(hole: tuple[int, int], ranges: list[tuple[int, int]]) -> list[tuple[int, int]]:
    pieces = [hole]
    for lo, hi in ranges:
        nxt: list[tuple[int, int]] = []
        for a, b in pieces:
            if hi < a or lo > b:
                nxt.append((a, b))
                continue
            if lo > a:
                nxt.append((a, lo - 1))
            if hi < b:
                nxt.append((hi + 1, b))
        pieces = nxt
    return pieces


def verify_lake(root: Path, *, check_hashes: bool = True) -> Report:
    root = Path(root)
    report = Report(root=str(root))
    issues = report.issues
    manifest = Manifest(root)
    m_events = manifest.read_all()
    live = manifest.live_segments()
    pruned = {e.get("name") for e in m_events if e.get("ev") == "prune"}
    ledger = Ledger.read_all(root)

    for e in m_events:
        if e.get("ev") == "corrupt_line":
            issues.append(Issue("MANIFEST_CORRUPT_LINE", FAIL, "-", detail={"file": e.get("file")}))
    for e in ledger:
        if e.get("k") == "CORRUPT_LINE":
            issues.append(Issue("LEDGER_CORRUPT_LINE", WARN, "-", detail={"file": e.get("file")}))

    # 1. segment integrity (against the manifest)
    total_bytes = 0
    for name, ev in sorted(live.items()):
        path = root / name
        if not path.exists():
            issues.append(Issue("MANIFEST_DANGLING", FAIL, name))
            continue
        size = path.stat().st_size
        total_bytes += size
        if size != ev.get("bytes"):
            issues.append(
                Issue("SIZE_MISMATCH", FAIL, name, detail={"manifest": ev.get("bytes"), "disk": size})
            )
        elif check_hashes and sha256_file(path) != ev.get("sha256"):
            issues.append(Issue("SHA_MISMATCH", FAIL, name))
    raw = root / "raw"
    if raw.exists():
        for fp in sorted(raw.rglob("*.jsonl.zst")):
            rel = fp.relative_to(root).as_posix()
            if rel not in live and rel not in pruned:
                issues.append(Issue("UNREGISTERED_SEGMENT", FAIL, rel))
        for fp in sorted(raw.rglob("*.jsonl.zst.part")):
            issues.append(Issue("OPEN_PART_FILE", INFO, fp.relative_to(root).as_posix()))
    report.segments = {"sealed": len(live), "bytes": total_bytes, "pruned": len(pruned)}

    # 2. record pass, snapshot class first so depth verifiers can interleave snapshots
    by_class: dict[tuple[str, str], list[str]] = {}
    for name, ev in live.items():
        by_class.setdefault((str(ev.get("venue")), str(ev.get("cls"))), []).append(name)
    snapshots: dict[tuple[str, str], list[Record]] = {}
    drivers: dict[str, _DepthDriver] = {}
    verifiers: dict[str, _Base] = {}
    bits: dict[int, bytearray] = {}
    run_info: dict[int, dict[str, Any]] = {}
    last_gen: dict[str, tuple[int, int]] = {}
    boundaries: list[tuple[str, int, int, int]] = []

    for key in sorted(by_class, key=lambda k: (k[1] != "snapshot", k)):
        venue, cls = key
        prev_order: tuple[int, int] | None = None
        for name in sorted(by_class[key]):
            path = root / name
            if not path.exists():
                continue
            ev = live[name]
            reader = SegmentReader(path)
            count = 0
            first_q = last_q = None
            for line in reader.lines():
                try:
                    rec = decode_record(line)
                except EnvelopeError as exc:
                    issues.append(Issue("BAD_LINE", FAIL, name, detail={"error": str(exc)}))
                    continue
                count += 1
                if prev_order is not None and rec.order <= prev_order:
                    issues.append(
                        Issue(
                            "ORDER_VIOLATION",
                            FAIL,
                            rec.stream,
                            rec.run,
                            rec.seq,
                            rec.gen,
                            {"prev": list(prev_order), "segment": name},
                        )
                    )
                prev_order = rec.order
                first_q = rec.seq if first_q is None else first_q
                last_q = rec.seq
                _mark(bits, rec.run, rec.seq)
                info = run_info.setdefault(
                    rec.run,
                    {"min_q": rec.seq, "max_q": rec.seq, "last_t": rec.wall_us, "tail": deque(maxlen=50_000)},
                )
                info["min_q"] = min(info["min_q"], rec.seq)
                info["max_q"] = max(info["max_q"], rec.seq)
                info["last_t"] = max(info["last_t"], rec.wall_us)
                info["tail"].append((rec.seq, rec.wall_us))
                if rec.fallback_reason:
                    issues.append(
                        Issue(
                            "FALLBACK_RECORD",
                            INFO,
                            rec.stream,
                            rec.run,
                            rec.seq,
                            rec.gen,
                            {"reason": rec.fallback_reason},
                        )
                    )
                gk = (rec.run, rec.gen)
                if (
                    cls not in ("snapshot", "ref") and last_gen.get(rec.stream) != gk
                ):  # REST-derived: no WS_OPEN
                    boundaries.append((rec.stream, rec.run, rec.gen, rec.seq))
                    last_gen[rec.stream] = gk
                if cls == "snapshot":
                    snapshots.setdefault((venue, rec.stream.rsplit(":", 1)[-1].lower()), []).append(rec)
                    stat = verifiers.setdefault(rec.stream, _SnapshotStats(rec.stream))
                    stat.feed(rec)
                    continue
                v = verifiers.get(rec.stream)
                if v is None:
                    v = _make_verifier(rec.stream)
                    if v is None:
                        v = _GenericStats(rec.stream)
                    verifiers[rec.stream] = v
                    if isinstance(v, DepthVerifier):
                        sym = rec.stream.partition(":")[2].split("@", 1)[0].lower()
                        drivers[rec.stream] = _DepthDriver(v, snapshots.get((venue, sym), []))
                drv = drivers.get(rec.stream)
                (drv.feed if drv is not None else v.feed)(rec)
            _check_segment(issues, name, ev, reader, count, first_q, last_q)
    for drv in drivers.values():
        drv.finish()
    for v in verifiers.values():
        if v.stream not in drivers:
            v.finish()
        issues.extend(v.issues)
        report.streams[v.stream] = v.summary()

    # 3. sequence holes per run (records and ledger events share one counter)
    stops = {e["run"] for e in ledger if e.get("k") == "PROC_STOP"}
    starts = {e["run"]: e for e in ledger if e.get("k") == "PROC_START"}
    for e in ledger:
        if isinstance(e.get("run"), int) and isinstance(e.get("q"), int) and e["q"] >= 0 and e["run"] >= 0:
            _mark(bits, e["run"], e["q"])
    overruns: list[tuple[int, int, int]] = [
        (e["run"], e["first_q"], e["last_q"])
        for e in ledger
        if e.get("k") == "OVERRUN" and isinstance(e.get("first_q"), int) and isinstance(e.get("last_q"), int)
    ]
    for e in m_events:  # data removed by retention leaves sequence holes that the manifest accounts for
        if e.get("ev") == "prune" and e.get("first") and e.get("last"):
            overruns.append((int(e["first"]["r"]), int(e["first"]["q"]), int(e["last"]["q"])))
    for run, b in sorted(bits.items()):
        info = run_info.get(run)
        q_min, q_max = b.find(1), b.rfind(1)
        clean = run in stops
        tail_zone = None
        if not clean and info and info["tail"]:
            cutoff = info["last_t"] - CRASH_TAIL_US
            tail_zone = next((q for q, t in info["tail"] if t >= cutoff), None)
        run_overruns = [(lo, hi) for r, lo, hi in overruns if r == run]
        for hole in _zero_runs(b, q_min, q_max):
            for a, z in _subtract(hole, run_overruns):
                if tail_zone is not None and a >= tail_zone:
                    issues.append(
                        Issue("CRASH_TAIL_HOLE", INFO, "-", run, a, None, {"to": z, "count": z - a + 1})
                    )
                else:
                    issues.append(
                        Issue(
                            "SEQ_HOLE",
                            FAIL,
                            "-",
                            run,
                            a,
                            None,
                            {"to": z, "count": z - a + 1, "run_clean": clean},
                        )
                    )
        report.runs[run] = {
            "clean": clean,
            "prev_clean": (starts.get(run) or {}).get("prev_clean"),
            "min_q": q_min,
            "max_q": q_max,
        }
        if not clean:
            issues.append(Issue("RUN_NOT_STOPPED_CLEANLY", WARN, "-", run))

    # 4. explain what the raw data proves, using the ledger only as an explanation
    _explain(issues, ledger, overruns)
    opens = {(e["run"], e["gen"], s) for e in ledger if e.get("k") == "WS_OPEN" for s in e.get("streams", [])}
    for stream, run, gen, q in boundaries:
        if (run, gen, stream) not in opens:
            issues.append(Issue("GEN_BOUNDARY_UNEXPLAINED", FAIL, stream, run, q, gen))
    return report


def _check_segment(
    issues: list[Issue],
    name: str,
    ev: dict[str, Any],
    reader: SegmentReader,
    count: int,
    first_q: int | None,
    last_q: int | None,
) -> None:
    if reader.error:
        issues.append(Issue("ZSTD_ERROR", FAIL, name, detail={"error": reader.error}))
    elif not reader.complete:
        if ev.get("truncated"):
            issues.append(Issue("SEGMENT_TRUNCATED_RECOVERED", INFO, name))
        else:
            issues.append(Issue("SEGMENT_INCOMPLETE", FAIL, name))
    if reader.trailing:
        issues.append(Issue("SEGMENT_TRAILING_DATA", FAIL, name))
    if reader.partial_tail:
        issues.append(Issue("SEGMENT_PARTIAL_LINE", FAIL, name))
    expected = ev.get("records")
    if expected is not None and expected != count:
        issues.append(
            Issue("RECORD_COUNT_MISMATCH", FAIL, name, detail={"manifest": expected, "read": count})
        )
    first, last = ev.get("first"), ev.get("last")
    if first and first_q is not None and first.get("q") != first_q:
        issues.append(
            Issue("FIRST_RECORD_MISMATCH", FAIL, name, detail={"manifest": first.get("q"), "read": first_q})
        )
    if last and last_q is not None and last.get("q") != last_q:
        issues.append(
            Issue("LAST_RECORD_MISMATCH", FAIL, name, detail={"manifest": last.get("q"), "read": last_q})
        )


def _explain(issues: list[Issue], ledger: list[dict[str, Any]], overruns: list[tuple[int, int, int]]) -> None:
    detected = {
        (e.get("run"), e.get("stream"), e.get("bad_q")) for e in ledger if e.get("k") == "GAP_DETECTED"
    }
    for iss in issues:
        if iss.severity != FAIL:
            continue
        if iss.code == "DEPTH_GAP" and (iss.run, iss.stream, iss.q) in detected:
            iss.severity, iss.explained_by = INFO, "GAP_DETECTED"
        elif iss.code in ("TRADE_ID_GAP", "AGGTRADE_ID_GAP", "MARK_CADENCE_GAP"):
            prev_q = iss.detail.get("prev_q")
            if (
                prev_q is not None
                and iss.q is not None
                and any(r == iss.run and prev_q < lo and hi < iss.q for r, lo, hi in overruns)
            ):
                iss.severity, iss.explained_by = INFO, "OVERRUN"


class _GenericStats(_Base):
    def feed(self, rec: Record) -> None:
        self.stats.add(rec)


class _SnapshotStats(_Base):
    def feed(self, rec: Record) -> None:
        self.stats.add(rec)
        obj = rec.data
        if not isinstance(obj, dict) or not isinstance(obj.get("lastUpdateId"), int):
            self._issue("SNAPSHOT_SCHEMA", FAIL, rec)
