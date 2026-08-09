#!/usr/bin/env python3
"""Cross-reference checker for docs/SPEC_INVARIANTS.md.

Given a set of symbol names (state names, field names, struct/function names that
SPEC_INVARIANTS.md records as having a single authoritative definition), this lists
every place the spec documents AND the ported C++ headers mention that symbol, so a
reviewer can confirm each site was updated consistently instead of relying on memory
across a 7000+ line changelog.

Searching the headers as well as the specs is the point, not a convenience: once an
invariant is ported to code (Layer 2), spec-vs-code drift is exactly the failure mode
this is meant to catch, and it is invisible if only the prose is searched.

This does not judge correctness -- it only surfaces "here are all the touch points for
this symbol"; a human (or an agent doing a narrowly-scoped check) still has to read each
one against the invariant's current authoritative definition in SPEC_INVARIANTS.md.

Exit codes (so this can gate CI):
    0  every checked symbol was found in at least one searched file
    1  a checked symbol was found nowhere -- the ledger has gone stale, or the symbol
       was renamed/removed on one side without the ledger being updated
    2  the ledger itself could not be parsed

Usage:
    python tools/spec_xref_check.py                       # check every registered symbol
    python tools/spec_xref_check.py is_exchange_final      # check only the given symbols
    python tools/spec_xref_check.py --list                # print the registered symbols and exit
    python tools/spec_xref_check.py --quiet               # only report problems (CI mode)
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
INVARIANTS_FILE = REPO_ROOT / "docs" / "SPEC_INVARIANTS.md"

# The spec prose and the code it has been ported into. A symbol registered in the
# ledger should appear in at least one of these; drift between them is the thing
# worth looking at.
SEARCH_FILES = [
    REPO_ROOT / "docs" / "SUBMITPORT_REAL_IMPLEMENTATION_SPEC.md",
    REPO_ROOT / "docs" / "BINANCE_PRIVATE_REST_L4_SPEC.md",
    # Every header the specs name as an owning definition site. The first version
    # of this list had only the two below, which left SubmitOutcome,
    # OrchestratorGate, SubmitPort, SubmitResponse, OrchestratorContext,
    # AuditRecord and SymbolRules entirely unsearched — a symbol can only drift
    # unnoticed in a file nobody looks at.
    REPO_ROOT / "native" / "include" / "hengyuan" / "durable_control_plane.hpp",
    REPO_ROOT / "native" / "include" / "hengyuan" / "order_lifecycle.hpp",
    REPO_ROOT / "native" / "include" / "hengyuan" / "live_submit_orchestrator.hpp",
    REPO_ROOT / "native" / "include" / "hengyuan" / "audit_trail.hpp",
    REPO_ROOT / "native" / "include" / "hengyuan" / "account_truth.hpp",
    REPO_ROOT / "native" / "include" / "hengyuan" / "order_tracker.hpp",
    REPO_ROOT / "native" / "include" / "hengyuan" / "durable_frame_codec.hpp",
    REPO_ROOT / "native" / "include" / "hengyuan" / "durable_audit_sink.hpp",
    # Phase 1 (docs/SPEC_INVARIANTS.md): the platform lock/fsync/tip-anchor
    # code durable_audit_sink.hpp used to carry directly (including the
    # flock/CreateFileA identifiers the ledger's own GCC-only finding names)
    # moved into this shared file when ControlPlaneLogSink started reusing
    # it -- without adding it here, that finding would silently stop being
    # checkable the moment the code it describes moved to a new home.
    REPO_ROOT / "native" / "include" / "hengyuan" / "durable_log_store.hpp",
    # Phase 4 (docs/SPEC_INVARIANTS.md): these two have existed since Phase 1
    # but were never added here -- Phase 4 puts genuinely new, load-bearing
    # symbols in both (KeyRotationPayload's codec family, ControlPlaneLogSink's
    # rotate_active_key()/finish_append()), the exact same "a symbol can only
    # drift unnoticed in a file nobody looks at" gap the durable_log_store.hpp
    # entry above was added to close.
    REPO_ROOT / "native" / "include" / "hengyuan" / "control_plane_frame_codec.hpp",
    REPO_ROOT / "native" / "include" / "hengyuan" / "control_plane_log_sink.hpp",
    # Phase 5 (docs/SPEC_INVARIANTS.md): export_worker.hpp is a new file whose
    # symbols (LastRemoteAckedTipStore, ExportRunStatus, etc.) would otherwise
    # be unsearchable the moment they're registered -- same "a symbol can only
    # drift unnoticed in a file nobody looks at" gap the durable_log_store.hpp/
    # control_plane_frame_codec.hpp entries above were added to close.
    REPO_ROOT / "native" / "include" / "hengyuan" / "export_worker.hpp",
    # Round D (docs/SPEC_INVARIANTS.md): the three Compaction*Wire types were
    # promoted from mac[32]-only markers to real named fields, and four brand
    # new headers carry their codec/I/O implementation -- the same "a symbol
    # can only drift unnoticed in a file nobody looks at" gap every entry
    # above was added to close.
    REPO_ROOT / "native" / "include" / "hengyuan" / "compaction_intent_codec.hpp",
    REPO_ROOT / "native" / "include" / "hengyuan" / "compaction_breadcrumb_io.hpp",
    REPO_ROOT / "native" / "include" / "hengyuan" / "windows_native_io.hpp",
    REPO_ROOT / "native" / "include" / "hengyuan" / "compaction_lease.hpp",
    REPO_ROOT / "native" / "include" / "hengyuan" / "compaction_intent_store.hpp",
    # key_ring.hpp has existed since Phase 0 but was never added here either --
    # Round D adds genuinely new, load-bearing symbols to it (RetireStatus,
    # PinnedKeyHandle, pin_key()), so this is the same pre-existing gap as
    # above, closed now that it's actually load-bearing.
    REPO_ROOT / "native" / "include" / "hengyuan" / "key_ring.hpp",
]

_LEDGER_SECTION_START = "## 不变量清单"
_LEDGER_SECTION_END = "## 已知的"

# Matches `symbol_name` inside backticks, restricted to identifier-shaped tokens
# (letters/digits/underscore, optional leading dot for file-extension-style symbols
# like `.xgc`, optional :: for scoped names like OrderState::AbortedPreSend).
_BACKTICK_SYMBOL_RE = re.compile(r"`(\.?[A-Za-z_][A-Za-z0-9_:]*)`")

# Symbols too generic to be useful as a search key on their own (would just match
# noise throughout the prose). Extend this if the ledger grows more single-word entries.
_STOPWORDS = {
    "Filled", "Cancelled", "Rejected", "Expired", "Submitting", "Corrupt",
    "IoError", "Started", "Found", "NotFound", "true", "false",
}


def _configure_utf8_output() -> None:
    """Spec files are UTF-8 and contain em-dashes / CJK punctuation; force both
    streams to match regardless of the host console's default codepage (e.g. cp936
    on zh-CN Windows), otherwise output is silently mangled into mojibake."""
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")


def extract_registered_symbols(invariants_text: str) -> list[str]:
    section_start = invariants_text.find(_LEDGER_SECTION_START)
    if section_start == -1:
        raise SystemExit(
            f"error: could not find the '{_LEDGER_SECTION_START}' section in {INVARIANTS_FILE}"
        )
    section_end = invariants_text.find(_LEDGER_SECTION_END, section_start)
    # Searching from section_start already guarantees ordering, but state it
    # explicitly: a silently-inverted slice would yield an empty symbol list, and an
    # empty list would make this whole check pass vacuously -- the exact failure mode
    # the ledger exists to prevent.
    if section_end != -1 and section_end <= section_start:
        raise SystemExit(
            f"error: '{_LEDGER_SECTION_END}' precedes '{_LEDGER_SECTION_START}' in "
            f"{INVARIANTS_FILE}; refusing to parse a malformed ledger"
        )
    section = invariants_text[section_start:section_end if section_end != -1 else None]

    seen: dict[str, None] = {}
    for match in _BACKTICK_SYMBOL_RE.finditer(section):
        symbol = match.group(1)
        if symbol in _STOPWORDS:
            continue
        seen[symbol] = None
    if not seen:
        raise SystemExit(
            f"error: parsed the ledger section in {INVARIANTS_FILE} but found no symbols; "
            f"this check would pass vacuously, so treating it as a failure"
        )
    return list(seen.keys())


def find_occurrences(symbol: str) -> dict[Path, list[tuple[int, str]]]:
    pattern = re.compile(re.escape(symbol))
    results: dict[Path, list[tuple[int, str]]] = {}
    for path in SEARCH_FILES:
        if not path.exists():
            continue
        hits: list[tuple[int, str]] = []
        for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
            if pattern.search(line):
                hits.append((lineno, line.strip()))
        if hits:
            results[path] = hits
    return results


def main() -> int:
    _configure_utf8_output()

    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "symbols",
        nargs="*",
        help="symbol names to check; default = every symbol registered in SPEC_INVARIANTS.md",
    )
    parser.add_argument(
        "--list", action="store_true",
        help="print the registered symbol list and exit, without searching",
    )
    parser.add_argument(
        "--quiet", action="store_true",
        help="suppress the per-occurrence listing; report only missing symbols and the summary",
    )
    args = parser.parse_args()

    if not INVARIANTS_FILE.exists():
        print(f"error: {INVARIANTS_FILE} not found", file=sys.stderr)
        return 2
    missing_search_files = [p for p in SEARCH_FILES if not p.exists()]
    if missing_search_files:
        # Not fatal on its own -- but silently searching fewer files than intended is
        # how a check quietly stops checking, so say so loudly.
        for p in missing_search_files:
            print(f"warning: search target missing, not searched: {p}", file=sys.stderr)

    registered = extract_registered_symbols(INVARIANTS_FILE.read_text(encoding="utf-8"))

    if args.list:
        for s in registered:
            print(s)
        return 0

    symbols = args.symbols or registered
    total_touch_points = 0
    not_found: list[str] = []

    for symbol in symbols:
        occurrences = find_occurrences(symbol)
        count = sum(len(hits) for hits in occurrences.values())
        total_touch_points += count
        if not occurrences:
            not_found.append(symbol)
            print(f"\n=== `{symbol}` — NOT FOUND in any searched file ===")
            continue
        if args.quiet:
            continue
        files_summary = ", ".join(
            f"{p.relative_to(REPO_ROOT).as_posix()}×{len(h)}" for p, h in occurrences.items()
        )
        print(f"\n=== `{symbol}` — {count} occurrence(s) across {files_summary} ===")
        for path, hits in occurrences.items():
            rel = path.relative_to(REPO_ROOT).as_posix()
            for lineno, text in hits:
                print(f"  {rel}:{lineno}: {text}")

    print(
        f"\n{len(symbols)} symbol(s) checked, {total_touch_points} total touch point(s) "
        f"across {len(SEARCH_FILES)} searched file(s)."
    )
    if not_found:
        print(
            f"\nFAIL: {len(not_found)} registered symbol(s) found nowhere: "
            f"{', '.join('`' + s + '`' for s in not_found)}\n"
            f"The ledger ({INVARIANTS_FILE.relative_to(REPO_ROOT).as_posix()}) is stale, or a "
            f"symbol was renamed/removed on one side without updating it. Fix the ledger or "
            f"restore the symbol.",
            file=sys.stderr,
        )
        return 1

    print(
        f"OK. Verify each touch point against its authoritative definition in "
        f"{INVARIANTS_FILE.relative_to(REPO_ROOT).as_posix()} before editing."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
