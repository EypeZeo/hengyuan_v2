#!/usr/bin/env python3
"""Closure-ledger checker for docs/AUDIT_CLOSURE_LEDGER.md (blueprint task GATE-01).

The ledger maps every old audit identifier (blueprint 4.2 / 4.3 / 4.4) to its current
file:symbol, test, merged PR and CI run, and adjudicates a status word with the six-segment
evidence chain of blueprint 4.1.2 (Code -> Contract -> Fault -> Test -> Runtime -> Budget).
This tool makes that ledger mechanically checkable, so a status word cannot drift away from
the repository without somebody noticing:

  * every code:/test:/doc:/open:/absent: reference is resolved against the working tree.
    A defect declared still open (open:/absent:) must still be found as declared -- the day a
    fix removes the symbol, the check fails and forces the ledger (and the blueprint) to move;
  * the adjudicated status must follow from the segment verdicts: CLOSED if and only if no
    segment is MISSING, NA only where the item kind allows it and only with a stated reason;
  * statuses, BKL rows and FI statuses must equal the blueprint's own tables;
  * with --online, every cited PR must be merged (merge-commit prefix matches) and every cited
    CI run must be completed with conclusion success on the cited commit and workflow -- a
    cancelled, failed or still-running run is never evidence (GATE-01's own completion
    condition names exactly this trap for the sanitizer workflow).

The tool judges whether the ledger is consistent with the repository; it does not judge
whether a fix is correct.

Exit codes (so this can gate CI later):
    0  the ledger is consistent
    1  at least one finding (stale reference, status not following from the evidence, ...)
    2  the ledger could not be parsed, or --online could not reach GitHub

Usage:
    python tools/closure_ledger_check.py                  # offline: working tree + blueprint
    python tools/closure_ledger_check.py --online         # also verify PRs and CI runs via gh
    python tools/closure_ledger_check.py --strict-fault   # refuse UT-grade Fault evidence
    python tools/closure_ledger_check.py --self-test      # negative controls for every check
    python tools/closure_ledger_check.py --emit-overview  # print the generated overview tables
    python tools/closure_ledger_check.py --quiet          # only report problems
"""

from __future__ import annotations

import argparse
import contextlib
import copy
import dataclasses
import functools
import json
import re
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
LEDGER_PATH = REPO_ROOT / "docs" / "AUDIT_CLOSURE_LEDGER.md"
FROZEN_PATH = (
    REPO_ROOT / "docs" / "archive" / "HengYuan_v2_全自动量化实盘系统实施指南与TODOLIST.orig-9d78643d.md"
)
REPO_SLUG = "EypeZeo/hengyuan_v2"

SEGMENTS = ("code", "contract", "fault", "test", "runtime", "budget")
STATUSES = ("REMEDIATING", "DEFERRED", "PARTIAL", "CLOSED")
VERDICTS = ("PRESENT", "NA", "MISSING")
GRADES = ("FI", "UT")

# Which segments an item kind may mark NA (always with a stated reason). Code and Contract are
# never NA. `live` (external I/O, clocks, crash recovery, money path) may not use NA at all.
KIND_NA: dict[str, frozenset[str]] = {
    "live": frozenset(),
    "lib": frozenset({"budget"}),
    "type": frozenset({"fault", "runtime", "budget"}),
    "build": frozenset({"fault", "budget"}),
    "doc": frozenset({"fault", "test", "runtime", "budget"}),
}

# Item status -> gate result. A DEFERRED item is an explicit, trigger-bound deferral and does
# not block its gate; REMEDIATING and PARTIAL do.
GATE_RESULT = {"CLOSED": "CLEARED", "DEFERRED": "DEFERRED", "REMEDIATING": "BLOCKED", "PARTIAL": "BLOCKED"}

TOKEN_KINDS = ("code", "open", "absent", "test", "doc", "bp", "frozen", "ignored", "pr", "run", "fi", "log")
# `countN:path#needle` declares that needle occurs exactly N times in the file.
_KINDS_ALT = "|".join(TOKEN_KINDS)
TOKEN_RE = re.compile(
    rf"""(?<![\w./-])(?P<kind>{_KINDS_ALT}|count\d+):"""
    r"""(?P<body>[^\s#"']+(?:#(?:"[^"]*"|'[^']*'|\S+))?|"[^"]*"|'[^']*')"""
)
RUN_RE = re.compile(r"^(\d+)@([0-9a-f]{7,40})/([\w.-]+\.ya?ml)(?:\[(.+)\])?$")
PR_RE = re.compile(r"^(\d+)@([0-9a-f]{7,40})$")
FI_RE = re.compile(r"^(FI-\d{3})=([A-Z_]+)$")

# Real runs that are NOT evidence, used by --online to prove the live path rejects them.
ONLINE_CONTROL_RUNS = (
    ("35556436339", "cancelled"),
    ("34801501343", "cancelled"),
    ("34078573391", "cancelled"),
    ("36810545639", "failure"),
)

OVERVIEW_BEGIN = "<!-- overview:begin -->"
OVERVIEW_END = "<!-- overview:end -->"


# --------------------------------------------------------------------------------------
# model
# --------------------------------------------------------------------------------------
@dataclasses.dataclass(frozen=True)
class Token:
    kind: str
    path: str  # file path / bare body for pr, run, fi, bp, frozen
    needle: str | None  # text after '#', quotes stripped
    quoted: bool
    raw: str


@dataclasses.dataclass
class Segment:
    verdict: str
    grade: str | None
    tokens: list[Token]
    text: str


@dataclasses.dataclass
class Item:
    id: str
    line: int
    fields: dict[str, list[str]]
    segments: dict[str, Segment]
    tokens: list[Token]  # open:/refs: lines (item level)

    def one(self, key: str) -> str:
        values = self.fields.get(key, [])
        return values[0].strip() if values else ""

    @property
    def status(self) -> str:
        return self.one("status")

    @property
    def kind(self) -> str:
        return self.one("kind")

    @property
    def all_tokens(self) -> list[Token]:
        out = list(self.tokens)
        for seg in self.segments.values():
            out.extend(seg.tokens)
        return out


@dataclasses.dataclass
class Finding:
    code: str
    item: str
    message: str

    def render(self) -> str:
        return f"[{self.code}] {self.item}: {self.message}"


class LedgerFormatError(Exception):
    pass


# --------------------------------------------------------------------------------------
# parsing
# --------------------------------------------------------------------------------------
def parse_tokens(text: str) -> list[Token]:
    tokens: list[Token] = []
    for m in TOKEN_RE.finditer(text):
        kind, body = m.group("kind"), m.group("body")
        path, needle, quoted = body, None, False
        if body[:1] in ('"', "'"):
            path = body[1:-1]
        elif "#" in body:
            path, _, needle = body.partition("#")
            if needle[:1] in ('"', "'"):
                needle, quoted = needle[1:-1], True
        tokens.append(Token(kind, path, needle, quoted, m.group(0)))
    return tokens


KEY_RE = re.compile(r"^([a-z][a-z0-9_.]*):\s?(.*)$")


def parse_ledger(text: str) -> tuple[dict[str, str], list[Item]]:
    """Return (front matter, items). Items are the ```ledger fenced blocks, in file order."""
    front: dict[str, str] = {}
    for line in text.splitlines():
        m = re.match(r"^<!--\s*ledger-meta\s+([a-z_]+)=(.*?)\s*-->\s*$", line)
        if m:
            front[m.group(1)] = m.group(2)
    items: list[Item] = []
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        if lines[i].strip() == "```ledger":
            start = i + 1
            j = start
            while j < len(lines) and lines[j].strip() != "```":
                j += 1
            if j >= len(lines):
                raise LedgerFormatError(f"line {i + 1}: unterminated ```ledger block")
            items.append(_parse_block(lines[start:j], start + 1))
            i = j
        i += 1
    if not items:
        raise LedgerFormatError("no ```ledger blocks found")
    return front, items


def _parse_block(block: list[str], first_line: int) -> Item:
    fields: dict[str, list[str]] = {}
    segments: dict[str, Segment] = {}
    tokens: list[Token] = []
    for offset, raw in enumerate(block):
        if not raw.strip():
            continue
        m = KEY_RE.match(raw)
        if not m:
            raise LedgerFormatError(f"line {first_line + offset}: cannot parse {raw[:60]!r}")
        key, value = m.group(1), m.group(2).strip()
        if key.startswith("seg."):
            name = key[4:]
            if name not in SEGMENTS:
                raise LedgerFormatError(f"line {first_line + offset}: unknown segment {name!r}")
            if name in segments:
                raise LedgerFormatError(f"line {first_line + offset}: duplicate segment {name!r}")
            segments[name] = _parse_segment(value, first_line + offset)
            continue
        fields.setdefault(key, []).append(value)
        if key in ("open", "refs"):
            tokens.extend(parse_tokens(value))
    item_id = (fields.get("id") or [""])[0]
    if not re.fullmatch(r"(P\d-\d{3}|SUPP-\d{3})", item_id):
        raise LedgerFormatError(f"line {first_line}: bad or missing id {item_id!r}")
    return Item(item_id, first_line, fields, segments, tokens)


def _parse_segment(value: str, line: int) -> Segment:
    head, _, rest = value.partition(" ")
    verdict, _, grade = head.partition(":")
    if verdict not in VERDICTS:
        raise LedgerFormatError(f"line {line}: bad verdict {verdict!r}")
    left, sep, text = rest.partition(" -- ")
    if not sep and rest.strip().startswith("--"):
        left, text = "", rest.strip()[2:]
    return Segment(verdict, grade or None, parse_tokens(left), text.strip())


# --------------------------------------------------------------------------------------
# blueprint model
# --------------------------------------------------------------------------------------
@dataclasses.dataclass
class Blueprint:
    text: str
    audit_status: dict[str, str]  # P*-nnn and SUPP-nnn -> status word
    bkl: dict[str, tuple[str, str]]  # BKL-nnn -> (source id, status)
    fi_status: dict[str, str]  # FI-nnn -> implementation status


def find_blueprint() -> Path:
    found = sorted(p for p in (REPO_ROOT / "docs").glob("HengYuan_v2_*TODOLIST.md") if p.is_file())
    if len(found) != 1:
        raise LedgerFormatError(f"expected exactly one blueprint under docs/, found {len(found)}")
    return found[0]


def parse_blueprint(text: str) -> Blueprint:
    audit: dict[str, str] = {}
    for m in re.finditer(r"^\|\s*\*\*(P\d-\d{3})\*\*\s*\|\s*[A-Z]+\s*\|\s*([A-Z]+)\s*\|", text, re.M):
        audit[m.group(1)] = m.group(2)
    for m in re.finditer(
        r"^\|\s*\*\*(SUPP-\d{3})\*\*\s*\|[^|]*\|\s*[A-Z]+\s*\|[^|]*\|\s*\**([A-Z]+)\**\s*\|", text, re.M
    ):
        audit[m.group(1)] = m.group(2)
    bkl: dict[str, tuple[str, str]] = {}
    for m in re.finditer(r"^\|\s*\*\*(BKL-\d{3})\*\*\s*\|\s*(P\d-\d{3})\s*\|\s*([A-Z]+)\s*\|", text, re.M):
        bkl[m.group(1)] = (m.group(2), m.group(3))
    fi: dict[str, str] = {}
    for m in re.finditer(r"^\|\s*\*\*(FI-\d{3})\*\*\s*\|[^|]*\|[^|]*\|[^|]*\|\s*([A-Z_]+)\s*\|", text, re.M):
        fi[m.group(1)] = m.group(2)
    return Blueprint(text, audit, bkl, fi)


# --------------------------------------------------------------------------------------
# file facts
# --------------------------------------------------------------------------------------
class Files:
    """Cached reads of repository files (utf-8, universal newlines)."""

    def __init__(self, root: Path) -> None:
        self.root = root
        self._cache: dict[str, str | None] = {}

    def text(self, rel: str) -> str | None:
        if rel not in self._cache:
            p = self.root / rel
            try:
                self._cache[rel] = p.read_text(encoding="utf-8", errors="replace") if p.is_file() else None
            except OSError:
                self._cache[rel] = None
        return self._cache[rel]

    def glob(self, pattern: str) -> list[str]:
        return sorted(
            str(p.relative_to(self.root)).replace("\\", "/") for p in self.root.glob(pattern) if p.is_file()
        )


def has_symbol(text: str, symbol: str) -> bool:
    """Whole-word symbol check; `A::B` requires B after the first occurrence of A."""
    pos = 0
    for part in symbol.split("::"):
        m = re.compile(rf"(?<![A-Za-z0-9_]){re.escape(part)}(?![A-Za-z0-9_])").search(text, pos)
        if not m:
            return False
        pos = m.end()
    return True


def needle_found(text: str, token: Token) -> bool:
    assert token.needle is not None
    if token.quoted or token.kind in ("doc", "absent", "frozen"):
        return token.needle in text
    return has_symbol(text, token.needle)


# --------------------------------------------------------------------------------------
# online facts
# --------------------------------------------------------------------------------------
class OnlineError(Exception):
    pass


class GhBackend:
    """PR and run facts through the gh CLI (the same transport the repo's own sessions use)."""

    def __init__(self, slug: str = REPO_SLUG) -> None:
        self.slug = slug
        self._cache: dict[str, dict] = {}

    def _api(self, path: str) -> dict:
        if path in self._cache:
            return self._cache[path]
        last = ""
        for attempt in range(4):
            proc = subprocess.run(
                ["gh", "api", path], capture_output=True, text=True, encoding="utf-8", errors="replace"
            )
            if proc.returncode == 0:
                data = json.loads(proc.stdout)
                self._cache[path] = data
                return data
            last = (proc.stderr or proc.stdout).strip().splitlines()[-1:] or ["?"]
            last = last[0][:160]
            if "404" in last or "Not Found" in last:
                raise OnlineError(f"{path}: not found")
            time.sleep(1.5 * (attempt + 1))
        raise OnlineError(f"{path}: gh api kept failing ({last})")

    def pr(self, number: int) -> dict:
        d = self._api(f"repos/{self.slug}/pulls/{number}")
        return {
            "merged": bool(d.get("merged")),
            "sha": d.get("merge_commit_sha") or "",
            "state": d.get("state"),
        }

    def run(self, run_id: int) -> dict:
        d = self._api(f"repos/{self.slug}/actions/runs/{run_id}")
        jobs = self._api(f"repos/{self.slug}/actions/runs/{run_id}/jobs?per_page=100")
        return {
            "status": d.get("status"),
            "conclusion": d.get("conclusion"),
            "head_sha": d.get("head_sha") or "",
            "workflow": (d.get("path") or "").split("@")[0].rsplit("/", 1)[-1],
            "jobs": [(j.get("name") or "", j.get("conclusion")) for j in jobs.get("jobs", [])],
        }


class FakeBackend:
    """Deterministic facts for --self-test; mutated per control."""

    def __init__(self, prs: dict[int, dict], runs: dict[int, dict]) -> None:
        self.prs, self.runs = prs, runs

    def pr(self, number: int) -> dict:
        if number not in self.prs:
            raise OnlineError(f"pr {number}: not found")
        return self.prs[number]

    def run(self, run_id: int) -> dict:
        if run_id not in self.runs:
            raise OnlineError(f"run {run_id}: not found")
        return self.runs[run_id]


def check_pr_token(tok: Token, item: str, backend) -> list[Finding]:
    m = PR_RE.match(tok.path)
    if not m:
        return [Finding("E-PR-FORMAT", item, f"bad pr token {tok.raw}")]
    number, prefix = int(m.group(1)), m.group(2)
    try:
        facts = backend.pr(number)
    except OnlineError as exc:
        return [Finding("E-PR", item, str(exc))]
    if not facts["merged"]:
        return [Finding("E-PR", item, f"PR #{number} is not merged (state={facts['state']})")]
    if not facts["sha"].startswith(prefix):
        return [
            Finding("E-PR", item, f"PR #{number} merge commit is {facts['sha'][:9]}, ledger says {prefix}")
        ]
    return []


def check_run_token(tok: Token, item: str, backend) -> list[Finding]:
    m = RUN_RE.match(tok.path)
    if not m:
        return [Finding("E-RUN-FORMAT", item, f"bad run token {tok.raw}")]
    run_id, prefix, workflow, job = int(m.group(1)), m.group(2), m.group(3), m.group(4)
    try:
        facts = backend.run(run_id)
    except OnlineError as exc:
        return [Finding("E-RUN", item, str(exc))]
    out: list[Finding] = []
    if facts["status"] != "completed":
        out.append(Finding("E-RUN-CONCLUSION", item, f"run {run_id} is {facts['status']}, not completed"))
    elif facts["conclusion"] != "success":
        out.append(
            Finding("E-RUN-CONCLUSION", item, f"run {run_id} concluded {facts['conclusion']}, not success")
        )
    if not facts["head_sha"].startswith(prefix):
        out.append(
            Finding("E-RUN-SHA", item, f"run {run_id} ran {facts['head_sha'][:9]}, ledger says {prefix}")
        )
    if facts["workflow"] != workflow:
        out.append(
            Finding("E-RUN-WORKFLOW", item, f"run {run_id} is {facts['workflow']}, ledger says {workflow}")
        )
    if job:
        hit = [c for name, c in facts["jobs"] if job in name]
        if not hit:
            out.append(Finding("E-RUN-JOB", item, f"run {run_id} has no job matching {job!r}"))
        elif any(c != "success" for c in hit):
            out.append(Finding("E-RUN-JOB", item, f"run {run_id} job {job!r} concluded {hit}, not success"))
    return out


# --------------------------------------------------------------------------------------
# checks
# --------------------------------------------------------------------------------------
@dataclasses.dataclass
class Report:
    findings: list[Finding] = dataclasses.field(default_factory=list)
    skipped: dict[str, int] = dataclasses.field(default_factory=dict)
    checked: dict[str, int] = dataclasses.field(default_factory=dict)

    def add(self, finding: Finding) -> None:
        self.findings.append(finding)

    def count(self, what: str) -> None:
        self.checked[what] = self.checked.get(what, 0) + 1

    def skip(self, what: str) -> None:
        self.skipped[what] = self.skipped.get(what, 0) + 1


@functools.cache
def git_ok(*args: str) -> bool:
    """True if `git <args>` exits 0. Cached: the repository does not change during one run."""
    return subprocess.run(["git", *args], cwd=REPO_ROOT, capture_output=True).returncode == 0


def check_path_token(tok: Token, item: str, files: Files, report: Report) -> None:
    report.count(tok.kind)
    if tok.kind == "ignored":
        if not git_ok("check-ignore", "-q", "--", tok.path):
            report.add(Finding("E-IGNORED", item, f"{tok.path} is not ignored by .gitignore"))
        return
    if tok.kind == "log":
        if files.text(tok.path) is None:
            report.add(Finding("E-FILE", item, f"log artifact {tok.path} does not exist"))
        return
    if tok.kind == "absent":
        paths = (
            files.glob(tok.path)
            if any(c in tok.path for c in "*?[")
            else ([tok.path] if files.text(tok.path) is not None else [])
        )
        if not paths:
            report.add(Finding("E-FILE", item, f"absent: path {tok.path} matches no file"))
            return
        if tok.needle is None:
            report.add(Finding("E-FORMAT", item, f"absent: token needs #needle: {tok.raw}"))
            return
        for rel in paths:
            if tok.needle in (files.text(rel) or ""):
                report.add(
                    Finding(
                        "E-ABSENT-HIT", item, f"{rel} contains {tok.needle!r} -- the ledger says it is absent"
                    )
                )
        return
    text = files.text(tok.path)
    if text is None:
        report.add(Finding("E-FILE", item, f"{tok.kind}: file {tok.path} does not exist"))
        return
    if tok.needle is None:
        report.add(Finding("E-FORMAT", item, f"{tok.kind}: token needs #needle: {tok.raw}"))
        return
    if tok.kind.startswith("count"):
        expected = int(tok.kind[len("count") :])
        actual = text.count(tok.needle)
        if actual != expected:
            report.add(
                Finding(
                    "E-COUNT",
                    item,
                    f"{tok.path} has {actual} x {tok.needle!r}, ledger says {expected} "
                    "-- the code moved: re-adjudicate this item",
                )
            )
        return
    if not needle_found(text, tok):
        code = "E-OPEN-STALE" if tok.kind == "open" else "E-SYMBOL"
        hint = (
            " -- the defect locus is gone: update the ledger and the blueprint" if tok.kind == "open" else ""
        )
        report.add(Finding(code, item, f"{tok.path} has no {tok.needle!r}{hint}"))


def check_item(item: Item, bp: Blueprint, files: Files, report: Report, *, strict_fault: bool) -> None:
    iid = item.id
    status, kind = item.status, item.kind
    for key in ("title", "severity", "kind", "status", "gate", "gate_result", "legacy"):
        if not item.one(key):
            report.add(Finding("E-FORMAT", iid, f"missing field {key}"))
    if status not in STATUSES:
        report.add(Finding("E-FORMAT", iid, f"bad status {status!r}"))
        return
    if kind not in KIND_NA:
        report.add(Finding("E-FORMAT", iid, f"bad kind {kind!r}"))
        return
    if item.one("gate_result") != GATE_RESULT[status]:
        report.add(
            Finding(
                "E-GATE", iid, f"gate_result {item.one('gate_result')!r} does not follow from status {status}"
            )
        )
    for name in SEGMENTS:
        if name not in item.segments:
            report.add(Finding("E-FORMAT", iid, f"missing segment {name}"))
    if len(item.segments) != len(SEGMENTS):
        return

    # segment rules
    missing: list[str] = []
    for name in SEGMENTS:
        seg = item.segments[name]
        kinds = {t.kind for t in seg.tokens}
        if seg.verdict == "MISSING":
            missing.append(name)
            if not seg.text:
                report.add(
                    Finding("E-MISSING-NO-TEXT", iid, f"{name} is MISSING without saying what is missing")
                )
        elif seg.verdict == "NA":
            if name in ("code", "contract") or name not in KIND_NA[kind]:
                report.add(Finding("E-NA-NOT-ALLOWED", iid, f"{name} may not be NA for kind {kind}"))
            if len(seg.text) < 6:
                report.add(Finding("E-NA-NO-REASON", iid, f"{name} is NA without a stated reason"))
        else:  # PRESENT
            if not seg.tokens:
                report.add(Finding("E-PRESENT-NO-REF", iid, f"{name} is PRESENT without any reference"))
        if name == "fault":
            if seg.verdict == "PRESENT":
                if seg.grade not in GRADES:
                    report.add(
                        Finding(
                            "E-GRADE", iid, "PRESENT fault evidence needs a grade (PRESENT:FI or PRESENT:UT)"
                        )
                    )
                elif seg.grade == "FI" and "fi" not in kinds:
                    report.add(Finding("E-GRADE", iid, "grade FI needs an fi: token"))
                elif seg.grade == "UT" and "test" not in kinds:
                    report.add(Finding("E-GRADE", iid, "grade UT needs a test: token"))
                elif seg.grade == "UT" and strict_fault:
                    report.add(
                        Finding(
                            "E-STRICT-FAULT", iid, "UT-grade fault evidence is refused under --strict-fault"
                        )
                    )
            elif seg.grade:
                report.add(Finding("E-GRADE", iid, "a grade only belongs on a PRESENT fault segment"))
        elif seg.grade:
            report.add(Finding("E-GRADE", iid, f"{name} carries a grade"))
        if name == "runtime" and seg.verdict == "PRESENT":
            if kind in ("lib", "build") and "run" not in kinds:
                report.add(
                    Finding("E-RUNTIME-EVIDENCE", iid, f"kind {kind} needs a run: token as runtime evidence")
                )
            if kind == "live" and "log" not in kinds:
                report.add(
                    Finding(
                        "E-RUNTIME-EVIDENCE",
                        iid,
                        "kind live needs a log: token (real, testnet or crash-recovery run)",
                    )
                )

    closable = not missing
    if status == "CLOSED" and not closable:
        report.add(Finding("E-STATUS", iid, f"CLOSED but segment(s) MISSING: {', '.join(missing)}"))
    if status != "CLOSED" and closable:
        report.add(
            Finding(
                "E-STATUS", iid, f"{status} although all six segments are PRESENT or NA -- adjudicate CLOSED"
            )
        )

    tokens = item.all_tokens
    kinds_all = {t.kind for t in tokens}
    open_like = [t for t in item.tokens if t.kind in ("open", "absent")]
    if status == "CLOSED":
        if any(t.kind == "open" for t in tokens):
            report.add(Finding("E-STATUS", iid, "CLOSED item still carries an open: token"))
        if "pr" not in kinds_all:
            report.add(Finding("E-STATUS", iid, "CLOSED item cites no merged PR"))
    elif not open_like:
        report.add(Finding("E-STATUS", iid, f"{status} item needs at least one open:/absent: locus token"))
    if status == "REMEDIATING":
        task = item.one("task")
        if not task:
            report.add(Finding("E-TASK", iid, "REMEDIATING item names no task"))
        else:
            for ref in re.findall(r"[A-Z]+-\d+(?:\.\d+)?|\d[A-Z]\.\d", task):
                if ref not in bp.text:
                    report.add(Finding("E-TASK", iid, f"task {ref} does not appear in the blueprint"))
    if status == "DEFERRED" and not item.one("trigger"):
        report.add(Finding("E-BKL", iid, "DEFERRED item states no unfreeze trigger"))

    # blueprint agreement
    if bp.audit_status.get(iid) != status:
        report.add(
            Finding(
                "E-BP-STATUS", iid, f"blueprint says {bp.audit_status.get(iid)!r}, ledger says {status!r}"
            )
        )
    for bkl in item.fields.get("bkl", []):
        for ref in bkl.split():
            src = bp.bkl.get(ref)
            if src is None:
                report.add(Finding("E-BKL", iid, f"{ref} is not in the blueprint's 4.4"))
            else:
                if src[0] != iid:
                    report.add(Finding("E-BKL", iid, f"{ref} belongs to {src[0]} in the blueprint"))
                if src[1] != status:
                    report.add(
                        Finding("E-BKL", iid, f"{ref} is {src[1]} in the blueprint, ledger says {status}")
                    )

    frozen = files.text(str(FROZEN_PATH.relative_to(REPO_ROOT)).replace("\\", "/")) or ""
    for legacy in item.fields.get("legacy", []):
        for ref in legacy.split():
            report.count("legacy")
            if ref not in frozen:
                report.add(Finding("E-LEGACY", iid, f"legacy id {ref} is not in the frozen v2.5.6 original"))

    # per-token checks (offline part)
    for tok in tokens:
        if tok.kind in ("pr", "run"):
            continue  # online / ancestry, handled by check_history / check_online
        if tok.kind == "fi":
            m = FI_RE.match(tok.path)
            if not m:
                report.add(Finding("E-FORMAT", iid, f"bad fi token {tok.raw}"))
            elif bp.fi_status.get(m.group(1)) != m.group(2):
                have = bp.fi_status.get(m.group(1))
                msg = f"{m.group(1)} is {have!r} in the blueprint, ledger says {m.group(2)!r}"
                report.add(Finding("E-FI-STATUS", iid, msg))
            report.count("fi")
            continue
        if tok.kind == "bp":
            report.count("bp")
            if tok.path not in bp.text:
                report.add(Finding("E-BP-REF", iid, f"blueprint has no text {tok.path!r}"))
            continue
        if tok.kind == "frozen":
            report.count("frozen")
            if tok.path not in frozen:
                report.add(Finding("E-LEGACY", iid, f"frozen original has no {tok.path!r}"))
            continue
        check_path_token(tok, iid, files, report)


def check_history(items: list[Item], report: Report) -> None:
    """Offline PR check: a cited merge commit that is present locally must be in HEAD's history."""
    seen: set[str] = set()
    for item in items:
        for tok in item.all_tokens:
            if tok.kind != "pr" or tok.raw in seen:
                continue
            seen.add(tok.raw)
            m = PR_RE.match(tok.path)
            if not m:
                report.add(Finding("E-PR-FORMAT", item.id, f"bad pr token {tok.raw}"))
                continue
            sha = m.group(2)
            if not git_ok("cat-file", "-e", f"{sha}^{{commit}}"):
                report.skip("pr-history (commit not in this clone)")
            elif not git_ok("merge-base", "--is-ancestor", sha, "HEAD"):
                report.add(Finding("E-PR-HISTORY", item.id, f"{sha} is not an ancestor of HEAD"))
            else:
                report.count("pr-history")


def check_online(items: list[Item], backend, report: Report) -> None:
    seen: set[str] = set()
    for item in items:
        for tok in item.all_tokens:
            if tok.kind not in ("pr", "run") or tok.raw in seen:
                continue
            seen.add(tok.raw)
            report.count("online-" + tok.kind)
            check = check_pr_token if tok.kind == "pr" else check_run_token
            for f in check(tok, item.id, backend):
                report.add(f)


def check_coverage(items: list[Item], bp: Blueprint, report: Report) -> None:
    ids = [i.id for i in items]
    if len(ids) != len(set(ids)):
        report.add(Finding("E-COVERAGE", "-", "duplicate ledger ids"))
    for audit_id in bp.audit_status:
        if audit_id not in ids:
            report.add(Finding("E-COVERAGE", audit_id, "blueprint lists it, the ledger has no block for it"))
    for i in ids:
        if i not in bp.audit_status:
            report.add(Finding("E-COVERAGE", i, "ledger block without a row in the blueprint's 4.2/4.3"))
    claimed = [r for it in items for b in it.fields.get("bkl", []) for r in b.split()]
    for bkl in bp.bkl:
        if claimed.count(bkl) != 1:
            report.add(
                Finding(
                    "E-COVERAGE",
                    bkl,
                    f"must be claimed by exactly one ledger item, claimed {claimed.count(bkl)}x",
                )
            )


# --------------------------------------------------------------------------------------
# overview (generated, then compared with the document)
# --------------------------------------------------------------------------------------
def render_overview(items: list[Item]) -> str:
    out = [
        "| 审计 ID | 等级 | 类别 | 状态 | 阻塞门禁 | 门禁结果 | 缺失段 |",
        "| :---: | :---: | :---: | :---: | :--- | :---: | :--- |",
    ]
    for it in items:
        miss = ", ".join(n for n in SEGMENTS if it.segments[n].verdict == "MISSING") or "—"
        out.append(
            f"| **{it.id}** | {it.one('severity')} | {it.kind} | {it.status} | {it.one('gate')} | "
            f"{it.one('gate_result')} | {miss} |"
        )
    gates: dict[str, list[Item]] = {}
    for it in items:
        gates.setdefault(it.one("gate"), []).append(it)
    out += ["", "| 门禁 | 结果 | 未关闭或暂缓的项 |", "| :--- | :---: | :--- |"]
    for gate in gates:
        group = gates[gate]
        open_ids = [i.id for i in group if i.status != "CLOSED"]
        verdict = (
            "BLOCKED"
            if any(i.status in ("REMEDIATING", "PARTIAL") for i in group)
            else ("DEFERRED" if open_ids else "CLEARED")
        )
        out.append(f"| {gate} | {verdict} | {', '.join(open_ids) or '—'} |")
    counts: dict[str, int] = {}
    for it in items:
        counts[it.status] = counts.get(it.status, 0) + 1
    out += [
        "",
        "状态计数：" + "，".join(f"{s} {counts.get(s, 0)}" for s in STATUSES) + f"，合计 {len(items)}。",
    ]
    return "\n".join(out)


def check_overview(ledger_text: str, items: list[Item], report: Report) -> None:
    b, e = ledger_text.find(OVERVIEW_BEGIN), ledger_text.find(OVERVIEW_END)
    if b < 0 or e < b:
        report.add(Finding("E-OVERVIEW", "-", "overview markers not found"))
        return
    have = ledger_text[b + len(OVERVIEW_BEGIN) : e].strip()
    want = render_overview(items).strip()
    if have != want:
        report.add(
            Finding(
                "E-OVERVIEW", "-", "the overview tables do not match the item blocks (run --emit-overview)"
            )
        )


# --------------------------------------------------------------------------------------
# driver
# --------------------------------------------------------------------------------------
def run_checks(
    ledger_text: str,
    front: dict[str, str],
    items: list[Item],
    bp: Blueprint,
    files: Files,
    backend,
    *,
    online: bool,
    strict_fault: bool,
    check_docs: bool = True,
) -> Report:
    report = Report()
    check_coverage(items, bp, report)
    for item in items:
        check_item(item, bp, files, report, strict_fault=strict_fault)
    baseline = front.get("baseline", "")
    if not baseline:
        report.add(Finding("E-FORMAT", "-", "ledger-meta baseline is missing"))
    elif git_ok("cat-file", "-e", f"{baseline}^{{commit}}") and not git_ok(
        "merge-base", "--is-ancestor", baseline, "HEAD"
    ):
        report.add(Finding("E-PR-HISTORY", "-", f"baseline {baseline} is not an ancestor of HEAD"))
    check_history(items, report)
    if online:
        check_online(items, backend, report)
    if check_docs:
        check_overview(ledger_text, items, report)
    return report


def build_fake_backend(items: list[Item]) -> FakeBackend:
    prs: dict[int, dict] = {}
    runs: dict[int, dict] = {}
    for item in items:
        for tok in item.all_tokens:
            if tok.kind == "pr" and (m := PR_RE.match(tok.path)):
                prs[int(m.group(1))] = {"merged": True, "sha": m.group(2) + "0" * 20, "state": "closed"}
            elif tok.kind == "run" and (m := RUN_RE.match(tok.path)):
                run = runs.setdefault(
                    int(m.group(1)),
                    {
                        "status": "completed",
                        "conclusion": "success",
                        "head_sha": m.group(2) + "0" * 20,
                        "workflow": m.group(3),
                        "jobs": [],
                    },
                )
                run["jobs"].append((m.group(4) or "any job", "success"))  # one run may be cited per job
    return FakeBackend(prs, runs)


def self_test(ledger_path: Path) -> int:
    """Negative controls: every check must fire on the defect it exists to catch.

    A check that never fails is not a check. Each control mutates a copy of the real ledger
    (or the blueprint model, or the fake online facts) and asserts the expected finding code.
    """
    text = ledger_path.read_text(encoding="utf-8")
    front, items = parse_ledger(text)
    bp = parse_blueprint(find_blueprint().read_text(encoding="utf-8"))
    files = Files(REPO_ROOT)
    failures: list[str] = []
    total = 0

    def run(its, blueprint=None, backend=None, strict=False):
        return run_checks(
            text,
            front,
            its,
            blueprint or bp,
            files,
            backend or build_fake_backend(its),
            online=True,
            strict_fault=strict,
            check_docs=False,
        )

    def verdict(name: str, good: bool, why: str = "") -> None:
        nonlocal total
        total += 1
        print(f"{name:<64} " + ("有效" if good else "无效" + (f"（{why}）" if why else "")))
        if not good:
            failures.append(name)

    def control(name, mutate, expect, *, bp_mutate=None, backend_mutate=None, strict=False) -> None:
        its = copy.deepcopy(items)
        mutate(its)
        blueprint = bp
        if bp_mutate:
            blueprint = copy.deepcopy(bp)
            bp_mutate(blueprint)
        backend = build_fake_backend(its)
        if backend_mutate:
            backend_mutate(backend)
        report = run(its, blueprint, backend, strict)
        verdict(name, any(f.code == expect for f in report.findings), f"没有报 {expect}")

    def item_of(its, **want) -> Item:
        for it in its:
            if all(getattr(it, k) == v for k, v in want.items()):
                return it
        raise LookupError(want)

    def token_of(its, kind, *, status=None, where=None) -> tuple[Item, Token]:
        for it in its:
            if status and it.status != status:
                continue
            for t in it.all_tokens:
                if t.kind == kind and (where is None or where(t)):
                    return it, t
        raise LookupError(kind)

    def swap(item: Item, old: Token, new: Token) -> None:
        item.tokens = [new if t is old else t for t in item.tokens]
        for seg in item.segments.values():
            seg.tokens = [new if t is old else t for t in seg.tokens]

    def drop_tokens(item: Item, kinds: set[str]) -> None:
        item.tokens = [t for t in item.tokens if t.kind not in kinds]
        for seg in item.segments.values():
            seg.tokens = [t for t in seg.tokens if t.kind not in kinds]

    clean = run(copy.deepcopy(items))
    verdict("干净账本：零发现", not clean.findings, f"{len(clean.findings)} 条发现")
    for f in clean.findings[:8]:
        print("   ", f.render())

    print("-- 引用必须能在工作树里解析 --")

    def m_file(its):
        it, t = token_of(its, "code")
        swap(it, t, dataclasses.replace(t, path="native/include/hengyuan/no_such_header.hpp"))

    control("code: 指向不存在的文件", m_file, "E-FILE")

    def m_symbol(its):
        it, t = token_of(its, "code")
        swap(it, t, dataclasses.replace(t, needle="NoSuchSymbolAnywhere", quoted=False))

    control("code: 指向不存在的符号", m_symbol, "E-SYMBOL")

    def m_substring(its):
        it, t = token_of(
            its, "test", where=lambda t: t.needle and t.needle.startswith("Rejects") and len(t.needle) > 12
        )
        swap(it, t, dataclasses.replace(t, needle=t.needle[:8], quoted=False))

    control("符号按整词匹配：子串不算命中", m_substring, "E-SYMBOL")

    def m_test(its):
        it, t = token_of(its, "test")
        swap(it, t, dataclasses.replace(t, needle="NoSuchTestCase", quoted=False))

    control("test: 指向不存在的测试", m_test, "E-SYMBOL")

    def m_doc(its):
        it, t = token_of(its, "doc")
        swap(it, t, dataclasses.replace(t, needle="一段从未写进文档的话", quoted=True))

    control("doc: 指向文档里不存在的文字", m_doc, "E-SYMBOL")

    def m_bp(its):
        it, t = token_of(its, "bp")
        swap(it, t, dataclasses.replace(t, path="蓝图里从未出现过的一句话"))

    control("bp: 指向蓝图里不存在的文字", m_bp, "E-BP-REF")

    def m_ignored(its):
        it, t = token_of(its, "ignored")
        swap(it, t, dataclasses.replace(t, path="docs/definitely-tracked-doc.md"))

    control("ignored: 路径其实没有被 .gitignore 忽略", m_ignored, "E-IGNORED")

    print("-- 缺陷位置声明：消失即失败 --")

    def m_open_stale(its):
        it, t = token_of(its, "open")
        swap(it, t, dataclasses.replace(t, needle="SymbolTheFixRemoved", quoted=False))

    control("open: 缺陷位置已消失（修复落地却没改账本）", m_open_stale, "E-OPEN-STALE")

    def m_absent_hit(its):
        it, t = token_of(its, "absent")
        rel = files.glob(t.path)[0] if any(c in t.path for c in "*?[") else t.path
        probe = re.findall(r"[A-Za-z_]{10,}", files.text(rel) or "")[0]
        swap(it, t, dataclasses.replace(t, needle=probe, quoted=True))

    control("absent: 声称不存在的东西其实存在", m_absent_hit, "E-ABSENT-HIT")

    def m_absent_path(its):
        it, t = token_of(its, "absent")
        swap(it, t, dataclasses.replace(t, path="native/no_such_dir/*.cpp"))

    control("absent: 路径一个文件也匹配不到（写错路径不得空过）", m_absent_path, "E-FILE")

    def m_count(its):
        it, t = token_of(its, "count4")
        swap(it, t, dataclasses.replace(t, kind="count5", raw=t.raw.replace("count4", "count5")))

    control("countN: 出现次数变了（兄弟位置新增了守卫）", m_count, "E-COUNT")

    print("-- 状态必须由六段判定推出 --")

    def m_closed_missing(its):
        it = item_of(its, status="REMEDIATING")
        it.fields["status"], it.fields["gate_result"] = ["CLOSED"], ["CLEARED"]

    control("缺段的缺陷被置为 CLOSED", m_closed_missing, "E-STATUS")

    def m_open_complete(its):
        it = item_of(its, status="CLOSED")
        it.fields["status"], it.fields["gate_result"] = ["PARTIAL"], ["BLOCKED"]

    control("六段齐全的缺陷仍停在未关闭", m_open_complete, "E-STATUS")

    def m_closed_no_pr(its):
        drop_tokens(item_of(its, status="CLOSED"), {"pr"})

    control("CLOSED 却不引用任何已合并 PR", m_closed_no_pr, "E-STATUS")

    def m_closed_open_token(its):
        it = item_of(its, status="CLOSED")
        it.tokens.append(Token("open", "README.md", "HengYuan", False, "open:README.md#HengYuan"))

    control("CLOSED 却仍带“缺陷仍在”的引用", m_closed_open_token, "E-STATUS")

    def m_open_no_locus(its):
        drop_tokens(item_of(its, status="REMEDIATING"), {"open", "absent"})

    control("未关闭项没有任何“缺陷仍在”的引用", m_open_no_locus, "E-STATUS")

    def m_gate(its):
        it = item_of(its, status="REMEDIATING")
        it.fields["gate_result"] = ["CLEARED"]

    control("门禁结果与状态不一致", m_gate, "E-GATE")

    def m_task(its):
        item_of(its, status="REMEDIATING").fields["task"] = ["SAFE-99"]

    control("REMEDIATING 的任务在蓝图里不存在", m_task, "E-TASK")

    def m_trigger(its):
        item_of(its, status="DEFERRED").fields["trigger"] = []

    control("DEFERRED 不写解冻条件", m_trigger, "E-BKL")

    print("-- 段的写法 --")

    def m_na_live(its):
        seg = item_of(its, kind="live").segments["fault"]
        seg.verdict, seg.text = "NA", "this is a long enough reason"

    control("live 类别对 Fault 段使用 NA", m_na_live, "E-NA-NOT-ALLOWED")

    def m_na_code(its):
        seg = item_of(its, kind="doc").segments["code"]
        seg.verdict, seg.text = "NA", "this is a long enough reason"

    control("Code 段使用 NA", m_na_code, "E-NA-NOT-ALLOWED")

    def m_na_reason(its):
        item_of(its, id="SUPP-006").segments["runtime"].text = ""

    control("NA 不写理由", m_na_reason, "E-NA-NO-REASON")

    def m_present_ref(its):
        item_of(its, status="CLOSED").segments["code"].tokens = []

    control("PRESENT 不带任何引用", m_present_ref, "E-PRESENT-NO-REF")

    def m_missing_text(its):
        it = item_of(its, status="REMEDIATING")
        next(s for s in it.segments.values() if s.verdict == "MISSING").text = ""

    control("MISSING 不说缺什么", m_missing_text, "E-MISSING-NO-TEXT")

    def m_grade_none(its):
        item_of(its, id="SUPP-004").segments["fault"].grade = None

    control("PRESENT 的 Fault 段没有分级", m_grade_none, "E-GRADE")

    def m_grade_ut_no_test(its):
        it = item_of(its, id="SUPP-004")
        it.segments["fault"].tokens = [t for t in it.segments["fault"].tokens if t.kind != "test"] + [
            Token("doc", "README.md", "HengYuan", False, "doc:README.md#HengYuan")
        ]

    control("UT 级 Fault 证据没有 test: 引用", m_grade_ut_no_test, "E-GRADE")

    control("--strict-fault 拒收 UT 级 Fault 证据", lambda its: None, "E-STRICT-FAULT", strict=True)

    def m_runtime_lib(its):
        it = item_of(its, id="SUPP-004")
        it.segments["runtime"].tokens = [
            Token("doc", "README.md", "HengYuan", False, "doc:README.md#HengYuan")
        ]

    control("lib 或 build 的 Runtime 段没有 CI run 凭据", m_runtime_lib, "E-RUNTIME-EVIDENCE")

    def m_runtime_live(its):
        it = item_of(its, kind="live")
        seg = it.segments["runtime"]
        seg.verdict, seg.text = "PRESENT", ""
        seg.tokens = [
            Token(
                "run",
                "37586509738@e75316caf/ci-native.yml",
                None,
                False,
                "run:37586509738@e75316caf/ci-native.yml",
            )
        ]

    control("live 的 Runtime 段只有 CI run（没有真实运行日志）", m_runtime_live, "E-RUNTIME-EVIDENCE")

    print("-- 与蓝图和冻结原文一致 --")

    def bp_status(b):
        b.audit_status["P0-001"] = "CLOSED"

    control("蓝图状态词与账本不一致", lambda its: None, "E-BP-STATUS", bp_mutate=bp_status)

    def bp_bkl(b):
        k = next(iter(b.bkl))
        b.bkl[k] = (b.bkl[k][0], "CLOSED")

    control("BKL 台账状态与来源项不一致", lambda its: None, "E-BKL", bp_mutate=bp_bkl)

    def bp_fi(b):
        b.fi_status["FI-001"] = "CHANGED_BY_CONTROL"

    control("FI 状态与蓝图注册表不一致", lambda its: None, "E-FI-STATUS", bp_mutate=bp_fi)

    def bp_extra(b):
        b.audit_status["P9-999"] = "REMEDIATING"

    control("蓝图有而账本没有的审计项", lambda its: None, "E-COVERAGE", bp_mutate=bp_extra)

    def m_dropped(its):
        its.pop()

    control("账本漏掉一项（覆盖不全）", m_dropped, "E-COVERAGE")

    def m_dup(its):
        its.append(copy.deepcopy(its[0]))

    control("账本出现重复的块", m_dup, "E-COVERAGE")

    def m_legacy(its):
        its[0].fields["legacy"] = ["NOT-IN-THE-FROZEN-ORIGINAL"]

    control("旧审计标识在冻结原文里查不到", m_legacy, "E-LEGACY")

    wrong = text.replace(OVERVIEW_BEGIN, OVERVIEW_BEGIN + "\n| 篡改 |", 1)
    rep = Report()
    check_overview(wrong, items, rep)
    verdict("总览表与明细不一致", any(f.code == "E-OVERVIEW" for f in rep.findings), "没有报 E-OVERVIEW")

    print("-- 线上事实（用确定性的假后端覆盖每一种失败形态；真实数据对照见 --online） --")
    first = copy.deepcopy(items)
    _, pr_tok = token_of(first, "pr")
    pr_no = int(PR_RE.match(pr_tok.path).group(1))
    _, run_tok = token_of(first, "run")
    run_no = int(RUN_RE.match(run_tok.path).group(1))
    noop = lambda its: None  # noqa: E731
    control(
        "pr: 未合并的 PR",
        noop,
        "E-PR",
        backend_mutate=lambda b: b.prs[pr_no].update(merged=False, state="open"),
    )
    control(
        "pr: 合并提交与账本不符", noop, "E-PR", backend_mutate=lambda b: b.prs[pr_no].update(sha="f" * 40)
    )
    control("pr: 不存在的 PR", noop, "E-PR", backend_mutate=lambda b: b.prs.pop(pr_no))
    control(
        "run: 被取消的 run",
        noop,
        "E-RUN-CONCLUSION",
        backend_mutate=lambda b: b.runs[run_no].update(conclusion="cancelled"),
    )
    control(
        "run: 失败的 run",
        noop,
        "E-RUN-CONCLUSION",
        backend_mutate=lambda b: b.runs[run_no].update(conclusion="failure"),
    )
    control(
        "run: 跳过的 run",
        noop,
        "E-RUN-CONCLUSION",
        backend_mutate=lambda b: b.runs[run_no].update(conclusion="skipped"),
    )
    control(
        "run: 尚未结束的 run",
        noop,
        "E-RUN-CONCLUSION",
        backend_mutate=lambda b: b.runs[run_no].update(status="in_progress", conclusion=None),
    )
    control(
        "run: 跑的不是账本写的提交",
        noop,
        "E-RUN-SHA",
        backend_mutate=lambda b: b.runs[run_no].update(head_sha="e" * 40),
    )
    control(
        "run: 不是账本写的 workflow",
        noop,
        "E-RUN-WORKFLOW",
        backend_mutate=lambda b: b.runs[run_no].update(workflow="other.yml"),
    )
    control("run: 不存在的 run", noop, "E-RUN", backend_mutate=lambda b: b.runs.pop(run_no))
    _, job_tok = token_of(first, "run", where=lambda t: RUN_RE.match(t.path).group(4))
    job_no = int(RUN_RE.match(job_tok.path).group(1))
    control(
        "run: 要求的作业不存在",
        noop,
        "E-RUN-JOB",
        backend_mutate=lambda b: b.runs[job_no].update(jobs=[("unrelated", "success")]),
    )
    control(
        "run: 要求的作业失败",
        noop,
        "E-RUN-JOB",
        backend_mutate=lambda b: b.runs[job_no].update(
            jobs=[(RUN_RE.match(job_tok.path).group(4), "failure")]
        ),
    )

    print("-- 账本语法 --")
    for name, mutate, fragment in (
        ("语法: 未知的判定词", lambda s: s.replace("seg.code: PRESENT", "seg.code: MAYBE", 1), "bad verdict"),
        (
            "语法: 重复的段",
            lambda s: s.replace("seg.budget: NA", "seg.code: NA\nseg.budget: NA", 1),
            "duplicate segment",
        ),
        ("语法: 未知的段", lambda s: s.replace("seg.budget:", "seg.cost:", 1), "unknown segment"),
        ("语法: 坏的审计 ID", lambda s: s.replace("id: P0-001", "id: X0-001", 1), "bad or missing id"),
        (
            "语法: 看不懂的行",
            lambda s: s.replace("severity: CRITICAL", "this line has no key", 1),
            "cannot parse",
        ),
    ):
        try:
            parse_ledger(mutate(text))
            verdict(name, False, "没有报错")
        except LedgerFormatError as exc:
            verdict(name, fragment in str(exc), str(exc)[:60])

    print()
    print(f"{total - len(failures)}/{total} 个对照有效。")
    if failures:
        print("无效的控制：", "; ".join(failures))
        return 1
    return 0


def main(argv: list[str] | None = None) -> int:
    for stream in (sys.stdout, sys.stderr):
        with contextlib.suppress(AttributeError, ValueError):
            stream.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--ledger", default=str(LEDGER_PATH), help="ledger path (default: docs/AUDIT_CLOSURE_LEDGER.md)"
    )
    parser.add_argument("--online", action="store_true", help="verify cited PRs and CI runs through gh")
    parser.add_argument("--strict-fault", action="store_true", help="refuse UT-grade Fault evidence")
    parser.add_argument("--self-test", action="store_true", help="run the negative controls")
    parser.add_argument("--emit-overview", action="store_true", help="print the generated overview tables")
    parser.add_argument("--quiet", action="store_true", help="only report problems")
    args = parser.parse_args(argv)

    ledger_path = Path(args.ledger)
    try:
        text = ledger_path.read_text(encoding="utf-8")
        front, items = parse_ledger(text)
        bp = parse_blueprint(find_blueprint().read_text(encoding="utf-8"))
    except (OSError, LedgerFormatError) as exc:
        print(f"cannot read the ledger: {exc}", file=sys.stderr)
        return 2

    if args.emit_overview:
        print(OVERVIEW_BEGIN)
        print(render_overview(items))
        print(OVERVIEW_END)
        return 0
    if args.self_test:
        return self_test(ledger_path)

    backend = None
    if args.online:
        backend = GhBackend()
        # prove the live path rejects real runs that are not evidence before trusting its "ok"
        for run_id, expect in ONLINE_CONTROL_RUNS:
            try:
                facts = backend.run(int(run_id))
            except OnlineError as exc:
                print(f"--online: cannot reach GitHub for control run {run_id}: {exc}", file=sys.stderr)
                return 2
            tok = Token(
                "run", f"{run_id}@{facts['head_sha'][:9]}/{facts['workflow']}", None, False, "control"
            )
            rejected = [f for f in check_run_token(tok, "control", backend) if f.code == "E-RUN-CONCLUSION"]
            if facts["conclusion"] != expect or not rejected:
                print(f"--online: control run {run_id} (expected {expect}) was not rejected", file=sys.stderr)
                return 1
            if not args.quiet:
                print(f"online control: run {run_id} ({expect}) rejected as designed")

    report = run_checks(
        text, front, items, bp, Files(REPO_ROOT), backend, online=args.online, strict_fault=args.strict_fault
    )
    for finding in report.findings:
        print(finding.render())
    if not args.quiet or report.findings:
        counts = {s: sum(1 for i in items if i.status == s) for s in STATUSES}
        print(f"items: {len(items)}  " + "  ".join(f"{s}={n}" for s, n in counts.items()))
        print("checked: " + ", ".join(f"{k}={v}" for k, v in sorted(report.checked.items())))
        if report.skipped:
            print("skipped: " + ", ".join(f"{k}={v}" for k, v in sorted(report.skipped.items())))
        if not args.online:
            print("online checks (PR merge state, CI run conclusions) not run; use --online")
        print(f"findings: {len(report.findings)}")
    return 1 if report.findings else 0


if __name__ == "__main__":
    sys.exit(main())
