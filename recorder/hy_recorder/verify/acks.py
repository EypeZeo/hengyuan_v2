"""Operator acknowledgements of known verify failures.

``verify`` re-derives every continuity fact from the raw records and fails a gap that the ledger does not
explain. One class of gap can never be explained by the ledger: the exchange stream itself lost messages while
the connection stayed healthy (the recorder cannot log what it was never sent). Such a FAIL is permanent: the
maintenance level stays CRIT, every daily run repeats the hour-long full verify, and a later, real FAIL only
changes a count.

An acknowledgement is the operator's statement "this exact gap was investigated and the lake itself is
intact". It is deliberately narrow:

* only the continuity codes in ``ACKABLE`` can be acknowledged; integrity, ordering, sequence-hole and depth
  failures mean the lake or the recorder is damaged and are never acknowledgeable;
* an ack matches ONE issue by its whole identity (code, stream, run, gen, q and every detail value), so a
  different or larger gap at the same place does not match and one entry cannot cover a second gap;
* a malformed line is itself a FAIL (``ACK_INVALID``) and an ack that matches nothing is a WARN
  (``ACK_UNMATCHED``): the file can neither hide something silently nor rot silently.

The file is ``<lake>/acks.jsonl``, one JSON object per line. The lake root lies outside ``raw/``,
``manifest/`` and ``ledger/``, so nothing the recorder or ``pull`` writes touches it.
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any

from .streams import FAIL, INFO, Issue

ACK_FILE = "acks.jsonl"
MAX_ACK_BYTES = 1 << 20
MIN_REASON_CHARS = 20
MAX_TEXT_CHARS = 2000

_ID_GAP_DETAIL = frozenset({"prev", "got", "missing", "prev_q"})
# code -> the detail keys the verifier attaches to it (``streams._IdChain._check`` and
# ``MarkPriceVerifier.feed``); ``test_verify_acks`` pins this against the real verifier output.
ACKABLE: dict[str, frozenset[str]] = {
    "TRADE_ID_GAP": _ID_GAP_DETAIL,
    "AGGTRADE_ID_GAP": _ID_GAP_DETAIL,
    "MARK_CADENCE_GAP": frozenset({"delta_ms", "prev_q"}),
}
_FIELDS = frozenset(
    {"v", "id", "code", "stream", "run", "gen", "q", "detail", "reason", "evidence", "by", "at"}
)
_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.:-]{0,63}$")
_AT_FORMAT = "%Y-%m-%dT%H:%M:%SZ"
_AT_RE = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$")


@dataclass(frozen=True)
class Ack:
    id: str
    code: str
    stream: str
    run: int
    gen: int
    q: int
    detail: dict[str, int]
    reason: str
    evidence: str
    by: str
    at: str
    line: int

    @property
    def key(self) -> tuple[str, str, int, int, int]:
        return (self.code, self.stream, self.run, self.gen, self.q)


def _is_int(v: Any) -> bool:
    return isinstance(v, int) and not isinstance(v, bool)


def _text(obj: dict[str, Any], name: str, *, min_chars: int, max_chars: int) -> str:
    v = obj[name]
    if not isinstance(v, str) or not min_chars <= len(v.strip()) <= max_chars:
        raise ValueError("%s must be text of %d to %d characters" % (name, min_chars, max_chars))
    return v


def _parse(obj: Any, line: int) -> Ack:
    """Validate one decoded line; raises ``ValueError`` naming the first problem."""
    if not isinstance(obj, dict):
        raise ValueError("not a JSON object")
    missing, unknown = sorted(_FIELDS - obj.keys()), sorted(obj.keys() - _FIELDS)
    if missing or unknown:
        raise ValueError("fields: missing %s, unknown %s" % (missing, unknown))
    if not _is_int(obj["v"]) or obj["v"] != 1:
        raise ValueError("v must be 1")
    if not isinstance(obj["id"], str) or not _ID_RE.match(obj["id"]):
        raise ValueError("id must match %s" % _ID_RE.pattern)
    code = obj["code"]
    if not isinstance(code, str) or code not in ACKABLE:
        raise ValueError("code must be one of %s" % ", ".join(sorted(ACKABLE)))
    stream = obj["stream"]
    if not isinstance(stream, str) or not 1 <= len(stream) <= 96:
        raise ValueError("stream must be text of 1 to 96 characters")
    for name in ("run", "gen", "q"):
        if not _is_int(obj[name]) or obj[name] < 0:
            raise ValueError("%s must be a non-negative integer" % name)
    detail = obj["detail"]
    if not isinstance(detail, dict) or set(detail) != ACKABLE[code]:
        raise ValueError("detail must have exactly the keys %s" % ", ".join(sorted(ACKABLE[code])))
    if not all(_is_int(v) for v in detail.values()):
        raise ValueError("detail values must be integers")
    reason = _text(obj, "reason", min_chars=MIN_REASON_CHARS, max_chars=MAX_TEXT_CHARS)
    evidence = _text(obj, "evidence", min_chars=1, max_chars=MAX_TEXT_CHARS)
    by = _text(obj, "by", min_chars=1, max_chars=64)
    at = obj["at"]
    if not isinstance(at, str) or not _AT_RE.match(at):
        raise ValueError("at must be a UTC time like 2026-10-19T08:00:00Z")
    datetime.strptime(at, _AT_FORMAT)  # a ValueError for an impossible calendar date
    return Ack(
        obj["id"],
        code,
        stream,
        obj["run"],
        obj["gen"],
        obj["q"],
        dict(detail),
        reason,
        evidence,
        by,
        at,
        line,
    )


def load_acks(path: Path) -> tuple[list[Ack], list[Issue]]:
    """Read the ack file: the valid entries, and one FAIL ``ACK_INVALID`` issue per problem found.

    A missing file is the normal state (no acknowledgements) and yields nothing. Anything that stops the
    whole file being read yields a FAIL and no entries: an unreadable file must never be read as "empty and
    fine" without anyone noticing.
    """
    path = Path(path)

    def bad(line: int | None, error: str) -> Issue:
        return Issue("ACK_INVALID", FAIL, "-", detail={"file": path.name, "line": line, "error": error})

    try:
        with path.open("rb") as fh:
            data = fh.read(MAX_ACK_BYTES + 1)
    except FileNotFoundError:
        return [], []
    except OSError as exc:
        return [], [bad(None, "%s: %s" % (type(exc).__name__, exc.strerror or "unreadable"))]
    if len(data) > MAX_ACK_BYTES:
        return [], [bad(None, "file is larger than %d bytes" % MAX_ACK_BYTES)]
    try:
        text = data.decode("utf-8-sig")
    except UnicodeDecodeError as exc:
        return [], [bad(None, "not UTF-8: %s" % exc)]
    acks: list[Ack] = []
    issues: list[Issue] = []
    ids: set[str] = set()
    keys: set[tuple[str, str, int, int, int]] = set()
    for n, raw in enumerate(text.split("\n"), start=1):
        if not raw.strip():
            continue
        try:
            ack = _parse(json.loads(raw), n)
            if ack.id in ids:
                raise ValueError("duplicate id %s" % ack.id)
            if ack.key in keys:
                raise ValueError("a second entry for the same gap as %s" % ack.id)
        except (ValueError, RecursionError) as exc:  # json.JSONDecodeError is a ValueError
            issues.append(bad(n, str(exc) if isinstance(exc, ValueError) else "nested too deeply"))
            continue
        ids.add(ack.id)
        keys.add(ack.key)
        acks.append(ack)
    return acks, issues


def apply_acks(issues: list[Issue], acks: list[Ack]) -> tuple[list[str], list[Ack]]:
    """Downgrade every FAIL issue that an ack matches exactly to INFO.

    Returns the ids that were applied (file order) and the acks that matched nothing. Only FAIL issues of an
    ackable code are looked at, so a gap the ledger already explained leaves its ack unmatched, and an ack is
    used at most once.
    """
    by_key = {a.key: a for a in acks}
    used: set[str] = set()
    for iss in issues:
        if iss.severity != FAIL or iss.code not in ACKABLE:
            continue
        ack = by_key.get((iss.code, iss.stream, iss.run, iss.gen, iss.q))
        if ack is None or ack.id in used or ack.detail != iss.detail:
            continue
        iss.severity, iss.explained_by = INFO, "ACK:" + ack.id
        used.add(ack.id)
    return [a.id for a in acks if a.id in used], [a for a in acks if a.id not in used]
