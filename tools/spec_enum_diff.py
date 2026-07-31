#!/usr/bin/env python3
"""Definition-level differ: spec `enum class` blocks vs. the real C++ headers.

WHY THIS EXISTS
---------------
tools/spec_xref_check.py verifies that a registered symbol NAME appears somewhere.
That is not enough. A port can use the right names and still be wrong in the way
that matters most: different enumerator values.

This tool was written after exactly that happened. A hand-written port of
native/include/hengyuan/durable_control_plane.hpp invented three types instead of
transcribing them from the spec:

  * DurableRecordType   9 invented enumerators with invented numbering, against
                        the spec's 17. It is a PERSISTED WIRE DISCRIMINATOR
                        covered by the frame MAC, so wrong numbering silently
                        reinterprets every frame already on disk.
  * RecoveryScanStatus  4 enumerators against the spec's 6, every value after
                        Clean shifted by two.
  * AuditAppendResult   a top-level DurableAppendStatus enum invented beside the
                        struct, where the spec nests `enum class Status`.

All three passed the full 381-test unit suite and passed spec_xref_check.py.
Only a value-level diff catches them.

WHAT IT DOES
------------
Extracts every `enum class X : <type> { ... };` from the ```cpp fenced blocks of
the spec documents, extracts the same from the headers, and compares enumerator
names and values pairwise.

DELIBERATELY SEPARATE FROM spec_xref_check.py: the two answer different questions
and need different exit-code policies. Merging them would make a single exit code
mean two incompatible things.

EXIT CODES
----------
    0  no conflicts (matches, not-yet-ported types, and spec-ahead warnings only)
    1  at least one VALUE_CONFLICT or NAME_CONFLICT — a real spec/code divergence
    2  the tool could not do its job (spec unreadable, zero enums extracted)

CLASSIFICATION, and why each severity is what it is
---------------------------------------------------
    VALUE_CONFLICT   same enumerator name, different number.  -> FAIL
                     The dangerous one: wire/ABI meaning silently changes.
    NAME_CONFLICT    header has an enumerator the spec does not.  -> FAIL
                     This is the signature of an invented type.
    MISSING_IN_CODE  spec has an enumerator the header does not.  -> WARN
                     Normal during incremental porting; failing on this would
                     make the tool unusable, since most spec types are partial.
    NOT_PORTED       the enum does not exist in any header at all.  -> silent
                     Roughly 50 of ~64 spec types are in this state by design.
    MATCH            identical name/value sets.  -> silent

Usage:
    python tools/spec_enum_diff.py            # check everything
    python tools/spec_enum_diff.py --verbose  # also list MATCH / NOT_PORTED
    python tools/spec_enum_diff.py --list     # dump what was extracted, then exit
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

SPEC_FILES = [
    REPO_ROOT / "docs" / "SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md",
    REPO_ROOT / "docs" / "BINANCE_PRIVATE_REST_L4_SPEC.md",
]

HEADER_DIR = REPO_ROOT / "native" / "include" / "hengyuan"

# A spec block that lists only NEW enumerators against an existing definition,
# e.g. OrchestratorGate's body opens with "// ... existing 18 values unchanged ...".
# Such a block is not a full definition; comparing it as one would report every
# unlisted enumerator as MISSING_IN_CODE. Detected structurally (see
# _DELTA_MARKER_RE) rather than by hardcoding the enum name, so a new delta block
# is handled automatically instead of silently mis-parsed.
_DELTA_MARKER_RE = re.compile(r"//.*\.\.\..*(existing|unchanged|as before)", re.IGNORECASE)

_FENCE_RE = re.compile(r"^```(\w*)\s*$")
_ENUM_OPEN_RE = re.compile(r"^\s*enum\s+class\s+(\w+)\s*:\s*([\w:]+)\s*\{\s*$")
# One-line form, e.g. the nested `enum class Status : std::uint8_t { Acked = 0, Failed = 1 };`
# inside struct AuditAppendResult.
_ENUM_ONELINE_RE = re.compile(r"^\s*enum\s+class\s+(\w+)\s*:\s*([\w:]+)\s*\{(.*)\}\s*;\s*$")
_ENUMERATOR_RE = re.compile(r"^\s*(\w+)\s*=\s*(-?\d+)\s*,?\s*$")


@dataclass
class EnumDef:
    name: str
    underlying: str
    source: str          # "docs/FOO.md:1234" or "native/include/hengyuan/x.hpp:12"
    values: dict[str, int] = field(default_factory=dict)
    is_delta: bool = False   # lists only additions, not a complete definition
    enclosing: str | None = None   # for nested enums: the struct/class name


def _strip_comment(line: str) -> str:
    """Remove a trailing // comment. Safe here: none of the enumerator lines in
    either spec or the headers contain a string literal with '//' in it."""
    idx = line.find("//")
    return line if idx == -1 else line[:idx]


def _parse_oneline_body(body: str) -> dict[str, int]:
    values: dict[str, int] = {}
    for part in body.split(","):
        part = _strip_comment(part).strip()
        if not part:
            continue
        m = re.match(r"^(\w+)\s*=\s*(-?\d+)$", part)
        if m:
            values[m.group(1)] = int(m.group(2))
    return values


def _extract_enums_from_lines(
    lines: list[str], origin: str, start_offset: int = 0
) -> list[EnumDef]:
    """Scan already-isolated C++ lines for enum class definitions.

    Tracks the innermost enclosing struct/class name so a nested enum can be
    reported as `Outer::Inner` — the spec nests AuditAppendResult::Status, and
    reporting it as a bare `Status` would collide with anything else so named.
    """
    out: list[EnumDef] = []
    # (name, brace_depth_at_open) for enclosing struct/class scopes
    scopes: list[tuple[str, int]] = []
    depth = 0
    i = 0
    while i < len(lines):
        raw = lines[i]
        line = _strip_comment(raw)

        one = _ENUM_ONELINE_RE.match(line)
        if one:
            name, underlying, body = one.group(1), one.group(2), one.group(3)
            out.append(EnumDef(
                name=name,
                underlying=underlying,
                source=f"{origin}:{start_offset + i + 1}",
                values=_parse_oneline_body(body),
                enclosing=scopes[-1][0] if scopes else None,
            ))
            i += 1
            continue

        opened = _ENUM_OPEN_RE.match(line)
        if opened:
            name, underlying = opened.group(1), opened.group(2)
            d = EnumDef(
                name=name,
                underlying=underlying,
                source=f"{origin}:{start_offset + i + 1}",
                enclosing=scopes[-1][0] if scopes else None,
            )
            j = i + 1
            while j < len(lines):
                body_raw = lines[j]
                if _DELTA_MARKER_RE.search(body_raw):
                    d.is_delta = True
                body = _strip_comment(body_raw)
                if body.strip().startswith("}"):
                    break
                em = _ENUMERATOR_RE.match(body)
                if em:
                    d.values[em.group(1)] = int(em.group(2))
                j += 1
            out.append(d)
            i = j + 1
            continue

        sm = re.match(r"^\s*(?:struct|class)\s+(\w+)", line)
        if sm and "{" in line and ";" not in line.split("{")[0]:
            scopes.append((sm.group(1), depth))

        depth += line.count("{") - line.count("}")
        while scopes and depth <= scopes[-1][1]:
            scopes.pop()
        i += 1
    return out


def extract_spec_enums() -> list[EnumDef]:
    found: list[EnumDef] = []
    for path in SPEC_FILES:
        if not path.exists():
            raise SystemExit(f"error: spec file not found: {path}")
        rel = path.relative_to(REPO_ROOT).as_posix()
        lines = path.read_text(encoding="utf-8").splitlines()
        in_block = False
        block: list[str] = []
        block_start = 0
        for idx, line in enumerate(lines):
            fence = _FENCE_RE.match(line)
            if fence:
                if not in_block:
                    # Only ```cpp blocks carry C++ definitions. The 4 untagged
                    # blocks across both specs are URL sketches and a math
                    # derivation; filtering on the tag loses nothing.
                    if fence.group(1) == "cpp":
                        in_block, block, block_start = True, [], idx + 1
                else:
                    found.extend(_extract_enums_from_lines(block, rel, block_start))
                    in_block = False
            elif in_block:
                block.append(line)
        if in_block:
            raise SystemExit(f"error: unterminated ``` block in {rel}; refusing to guess")
    return found


def extract_header_enums() -> list[EnumDef]:
    found: list[EnumDef] = []
    if not HEADER_DIR.is_dir():
        raise SystemExit(f"error: header directory not found: {HEADER_DIR}")
    for path in sorted(HEADER_DIR.glob("*.hpp")):
        rel = path.relative_to(REPO_ROOT).as_posix()
        found.extend(
            _extract_enums_from_lines(path.read_text(encoding="utf-8").splitlines(), rel)
        )
    return found


def qualified(e: EnumDef) -> str:
    return f"{e.enclosing}::{e.name}" if e.enclosing else e.name


@dataclass
class Finding:
    kind: str
    enum: str
    detail: str
    spec_src: str
    code_src: str


def diff(spec: list[EnumDef], code: list[EnumDef]) -> tuple[list[Finding], dict[str, int]]:
    # Index headers by both qualified and bare name: the spec nests
    # AuditAppendResult::Status while a header might reasonably reach it either way.
    code_by_name: dict[str, EnumDef] = {}
    for e in code:
        code_by_name.setdefault(qualified(e), e)
        code_by_name.setdefault(e.name, e)

    findings: list[Finding] = []
    tally = {"MATCH": 0, "NOT_PORTED": 0, "MISSING_IN_CODE": 0,
             "VALUE_CONFLICT": 0, "NAME_CONFLICT": 0, "DELTA_SKIPPED": 0}

    for s in spec:
        key = qualified(s)
        c = code_by_name.get(key) or code_by_name.get(s.name)
        if c is None:
            tally["NOT_PORTED"] += 1
            findings.append(Finding("NOT_PORTED", key, "not present in any header",
                                    s.source, "-"))
            continue

        conflicted = False
        for enum_name, spec_val in s.values.items():
            if enum_name in c.values and c.values[enum_name] != spec_val:
                tally["VALUE_CONFLICT"] += 1
                conflicted = True
                findings.append(Finding(
                    "VALUE_CONFLICT", key,
                    f"`{enum_name}`: spec = {spec_val}, code = {c.values[enum_name]}",
                    s.source, c.source))

        # A delta block lists only additions, so "in spec, not in code" is
        # meaningless for it and "in code, not in spec" would flag every
        # pre-existing enumerator. Only value conflicts are checkable.
        if s.is_delta:
            tally["DELTA_SKIPPED"] += 1
            findings.append(Finding(
                "DELTA_SKIPPED", key,
                "spec block lists additions only (\"... existing ... unchanged ...\"); "
                "compared for value conflicts only",
                s.source, c.source))
            continue

        for enum_name in s.values:
            if enum_name not in c.values:
                tally["MISSING_IN_CODE"] += 1
                findings.append(Finding(
                    "MISSING_IN_CODE", key,
                    f"`{enum_name} = {s.values[enum_name]}` in spec, absent from code",
                    s.source, c.source))
        for enum_name in c.values:
            if enum_name not in s.values:
                tally["NAME_CONFLICT"] += 1
                conflicted = True
                findings.append(Finding(
                    "NAME_CONFLICT", key,
                    f"`{enum_name} = {c.values[enum_name]}` in code has no spec basis",
                    s.source, c.source))

        if not conflicted and len(s.values) == len(c.values):
            tally["MATCH"] += 1

    return findings, tally


def check_cross_spec_consistency(spec: list[EnumDef]) -> list[Finding]:
    """Some enums are defined in BOTH spec documents (FrameTimeKind is, identically).
    Those copies must agree with each other — a free consistency check the spec's
    own review rounds had to do by eye."""
    by_name: dict[str, list[EnumDef]] = {}
    for e in spec:
        by_name.setdefault(qualified(e), []).append(e)
    out: list[Finding] = []
    for name, defs in by_name.items():
        if len(defs) < 2:
            continue
        first = defs[0]
        for other in defs[1:]:
            if first.values != other.values:
                out.append(Finding(
                    "VALUE_CONFLICT", name,
                    f"the two spec copies disagree: {first.values} vs {other.values}",
                    first.source, other.source))
    return out


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")

    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--verbose", action="store_true",
                    help="also list MATCH and NOT_PORTED entries")
    ap.add_argument("--list", action="store_true",
                    help="dump every extracted enum and exit without diffing")
    args = ap.parse_args()

    spec_enums = extract_spec_enums()
    code_enums = extract_header_enums()

    # Refuse to pass vacuously. If a spec reformat ever breaks the parser, an
    # empty extraction would otherwise report "no conflicts" forever.
    if not spec_enums:
        print("error: extracted 0 enums from the spec documents. The parser is broken "
              "or the specs changed shape; refusing to report success vacuously.",
              file=sys.stderr)
        return 2

    if args.list:
        print(f"--- {len(spec_enums)} enum(s) in specs ---")
        for e in spec_enums:
            tag = " [delta]" if e.is_delta else ""
            print(f"  {qualified(e):45} {e.source}{tag}  {len(e.values)} value(s)")
        print(f"\n--- {len(code_enums)} enum(s) in headers ---")
        for e in code_enums:
            print(f"  {qualified(e):45} {e.source}  {len(e.values)} value(s)")
        return 0

    findings, tally = diff(spec_enums, code_enums)
    findings.extend(check_cross_spec_consistency(spec_enums))

    fail_kinds = {"VALUE_CONFLICT", "NAME_CONFLICT"}
    warn_kinds = {"MISSING_IN_CODE"}
    quiet_kinds = {"MATCH", "NOT_PORTED"}

    failures = [f for f in findings if f.kind in fail_kinds]
    warnings = [f for f in findings if f.kind in warn_kinds]
    notes = [f for f in findings if f.kind == "DELTA_SKIPPED"]

    if failures:
        print("\n=== CONFLICTS (build-failing) ===")
        for f in failures:
            print(f"  [{f.kind}] {f.enum}: {f.detail}")
            print(f"      spec: {f.spec_src}")
            print(f"      code: {f.code_src}")

    if warnings:
        print("\n=== spec ahead of code (warning only) ===")
        for f in warnings:
            print(f"  [{f.kind}] {f.enum}: {f.detail}")

    if notes:
        print("\n=== delta blocks (value-conflicts checked, membership not) ===")
        for f in notes:
            print(f"  {f.enum}  ({f.spec_src})")

    if args.verbose:
        for f in findings:
            if f.kind in quiet_kinds:
                print(f"  [{f.kind}] {f.enum}  spec={f.spec_src} code={f.code_src}")

    print(
        f"\n{len(spec_enums)} spec enum(s) vs {len(code_enums)} header enum(s): "
        f"{tally['MATCH']} match, {tally['NOT_PORTED']} not yet ported, "
        f"{tally['MISSING_IN_CODE']} missing-in-code, "
        f"{tally['VALUE_CONFLICT']} value conflict(s), "
        f"{tally['NAME_CONFLICT']} name conflict(s)."
    )

    if failures:
        print(
            f"\nFAIL: {len(failures)} spec/code definition conflict(s). A VALUE_CONFLICT on a "
            f"persisted wire discriminator silently reinterprets stored data; a NAME_CONFLICT "
            f"means the header has an enumerator with no basis in the spec. Transcribe the "
            f"spec block verbatim rather than adjusting the spec to match the code.",
            file=sys.stderr,
        )
        return 1

    print("OK: no spec/code enum definition conflicts.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
