"""Regression pin for AUDIT TZ-POSTURE-DEAD-058 (批次 4 修复).

``py_core.manual_ohlcv`` has no dedicated test file of its own (a separate, broader
coverage gap noted by the audit but out of this fix's scope) -- this file exists narrowly
to pin the specific finding: ``TimestampPosture.NAIVE_SOURCE_TIMEZONE_APPLIED`` and
``OhlcvValidationIssueCode.MIXED_TIMESTAMP_POSTURE`` are legal schema values that no
current code path ever produces. That is a documented, intentional state (see the
docstrings on those two enum members), not a bug -- but it is exactly the kind of fact that
silently stops being true the moment someone adds a new manifest-producing import path
without also updating the manifest's ``timestamp_posture``. This test fails loudly if that
happens without anyone noticing, rather than leaving the enum "reserved" forever by
convention alone.
"""

from __future__ import annotations

import re
from pathlib import Path

from py_core.manual_ohlcv import OhlcvValidationIssueCode, TimestampPosture

_PY_CORE_ROOT = Path(__file__).resolve().parent.parent


def _production_files() -> list[Path]:
    return [
        p
        for p in _PY_CORE_ROOT.rglob("*.py")
        if "tests" not in p.parts and ".venv" not in p.parts and "__pycache__" not in p.parts
    ]


def _count_references(pattern: re.Pattern[str]) -> int:
    total = 0
    for path in _production_files():
        text = path.read_text(encoding="utf-8")
        total += len(pattern.findall(text))
    return total


def test_naive_source_timezone_applied_is_still_never_produced() -> None:
    """If this starts failing, a new producer now writes NAIVE_SOURCE_TIMEZONE_APPLIED --
    good news (the dead branch is live), but go re-read AUDIT TZ-POSTURE-DEAD-058's
    reasoning in manual_ohlcv.py before assuming the new producer's provenance semantics
    are actually correct, since nothing has ever exercised this path before."""
    pattern = re.compile(r"TimestampPosture\.NAIVE_SOURCE_TIMEZONE_APPLIED")
    # Zero references in production code today -- the only producer of
    # ManualOhlcvImportManifest (market_data/cli.py) hardcodes OFFSET_PROVIDED_IN_PAYLOAD.
    assert _count_references(pattern) == 0


def test_mixed_timestamp_posture_issue_code_is_still_never_emitted() -> None:
    """Same reasoning as above for the validation issue code -- no per-record posture
    tracking exists today, so nothing can ever detect or emit this."""
    pattern = re.compile(r"OhlcvValidationIssueCode\.MIXED_TIMESTAMP_POSTURE")
    assert _count_references(pattern) == 0


def test_timestamp_posture_and_issue_code_enum_members_still_exist() -> None:
    """Sanity check the two symbols above didn't just get renamed out from under this
    grep-based test."""
    assert TimestampPosture.NAIVE_SOURCE_TIMEZONE_APPLIED == "naive_source_timezone_applied"
    assert OhlcvValidationIssueCode.MIXED_TIMESTAMP_POSTURE == "MIXED_TIMESTAMP_POSTURE"
