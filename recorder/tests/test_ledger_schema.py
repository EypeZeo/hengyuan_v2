"""Ledger events share one sequence space with records; an event must never lose its own ``q``."""

from __future__ import annotations

import ast
from pathlib import Path

from hy_recorder.ledger import RESERVED_KEYS, make_event

PKG = Path(__file__).resolve().parents[1] / "hy_recorder"
EMIT_NAMES = {"emit", "_emit", "_ledger"}


def test_envelope_keys_cannot_be_overwritten_by_fields():
    ev = make_event(3, 41, 1_790_000_000_000_000, 5, "SNAPSHOT", q=7, t=8, k="other", stream="x")
    assert (ev["run"], ev["q"], ev["t"], ev["m"], ev["k"]) == (3, 41, 1_790_000_000_000_000, 5, "SNAPSHOT")
    assert ev["x_q"] == 7 and ev["x_t"] == 8 and ev["x_k"] == "other" and ev["stream"] == "x"
    assert ev["schema_clash"] == ["k", "q", "t"]


def test_clean_events_carry_no_clash_marker():
    assert "schema_clash" not in make_event(1, 1, 1, 1, "PROC_START", version="x")


def test_no_emit_call_site_uses_a_reserved_field_name():
    """Every ``emit("KIND", ...)``, ``self._emit(...)`` and ``self._ledger(...)`` in the package, by AST."""
    offenders: list[str] = []
    sites = 0
    for path in sorted(PKG.rglob("*.py")):
        tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
        for node in ast.walk(tree):
            if not isinstance(node, ast.Call):
                continue
            fn = node.func
            name = fn.attr if isinstance(fn, ast.Attribute) else fn.id if isinstance(fn, ast.Name) else None
            if name not in EMIT_NAMES or not node.args:
                continue
            first = node.args[0]
            if not (
                isinstance(first, ast.Constant) and isinstance(first.value, str) and first.value.isupper()
            ):
                continue
            sites += 1
            for kw in node.keywords:
                if kw.arg in RESERVED_KEYS:
                    offenders.append(
                        "%s:%d %s(%s, %s=...)" % (path.name, node.lineno, name, first.value, kw.arg)
                    )
    assert sites > 30, (
        "the scan found suspiciously few emit call sites (%d): the test itself is broken" % sites
    )
    assert offenders == [], "\n".join(offenders)
