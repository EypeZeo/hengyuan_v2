#!/usr/bin/env python3
"""Guard: every CMake test target labelled `concurrency` must actually be built by the sanitizer jobs.

`gtest_discover_tests` registers a target's tests only once the target has been built, and the TSan and
ARM64 jobs of .github/workflows/ci-native-sanitizers.yml build an explicit `--target` list. A target that
carries `LABELS concurrency` in native/CMakeLists.txt but is not named in that list is never built, so
`ctest -L concurrency` silently never runs it: no error, no warning, just a detector that does not look.
That is how seven concurrency tests went unobserved by CI's ThreadSanitizer (found 2026-10-08, first noticed
2026-09-20). This tool turns the hand-kept lists into something that can fail.

Rules (CMakeLists.txt is the source of truth for what is labelled; the YAML for what CI builds):
  * every labelled target is in the TSan job's list, or in TSAN_EXEMPT with a written reason;
  * every labelled target is in the ARM64 job's list, or in ARM64_EXEMPT with a written reason - the ARM64 job
    is the hardware weak-memory check, so a test that is mutex-guarded or file-lease based has no business
    there and says so;
  * a helper binary that a labelled test spawns (`$<TARGET_FILE:helper>` in its compile definitions) is built
    wherever the test is;
  * tools/wsl_verify.sh's `thread` tier builds exactly what the TSan job builds (the YAML is authoritative);
  * every name in a list exists in CMakeLists.txt; no exemption is stale (the target is not labelled, or is
    listed anyway).

Read-only. Exit codes: 0 consistent / 1 findings / 2 a file could not be parsed.

Usage:
    python tools/concurrency_targets_check.py              # check the repository
    python tools/concurrency_targets_check.py --list       # print the sets that were compared
    python tools/concurrency_targets_check.py --self-test  # negative controls for every rule
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
CMAKE = REPO_ROOT / "native" / "CMakeLists.txt"
WORKFLOW = REPO_ROOT / ".github" / "workflows" / "ci-native-sanitizers.yml"
WSL_VERIFY = REPO_ROOT / "tools" / "wsl_verify.sh"

TSAN_JOB = "tsan-concurrency"
ARM_JOB = "arm64-weak-memory"

# target -> reason. Keep reasons honest: a stale or empty one fails the guard.
# The TSan job is the broad net, so nothing is exempt from it. The ARM64 job is a hardware weak-memory
# check: it earns a target only if the code under test hands data across threads through a lock-free
# protocol (acquire/release mailbox, seqlock, SpscRing payload checksums), so lock-based and file-lease
# tests are listed here with the reason.
TSAN_EXEMPT: dict[str, str] = {}
_FILE_LEASE = (
    "real-filesystem (L2) test of the frozen seal-journal lease family; its cross-thread / cross-process "
    "exclusion is the OS file lock, not a memory-order protocol, so weak hardware reorders nothing here "
    "that the x86 and TSan runs do not already exercise"
)
ARM64_EXEMPT: dict[str, str] = {
    "test_symbol_registry_concurrency": (
        "std::shared_mutex only: the ordering is the lock's, and the reader/writer logic is covered by "
        "the TSan job; there is no lock-free handoff for a weak-memory run to observe"
    ),
    "test_compaction_lease": _FILE_LEASE,
    "test_compaction_intent_store": _FILE_LEASE,
    "test_seal_journal_store_lease": _FILE_LEASE,
    "test_intent_phase_advancer": _FILE_LEASE,
    "test_migrated_v2_started_publisher": _FILE_LEASE,
}

LABEL_RE = re.compile(
    r"gtest_discover_tests\(\s*([A-Za-z0-9_]+)\b[^)]*?\bLABELS\s+\"?concurrency\"?[^)]*\)", re.S
)
TARGET_FILE_RE = re.compile(r"\$<TARGET_FILE:([A-Za-z0-9_]+)>")


@dataclass
class Finding:
    code: str
    subject: str
    message: str

    def render(self) -> str:
        return f"[{self.code}] {self.subject}: {self.message}"


class ParseError(Exception):
    pass


def labelled_targets(cmake: str) -> dict[str, set[str]]:
    """Labelled target -> the helper binaries its compile definitions reference."""
    out: dict[str, set[str]] = {}
    for m in LABEL_RE.finditer(cmake):
        target = m.group(1)
        start = cmake.find(f"add_executable({target} ")
        if start < 0:
            raise ParseError(f"labelled target {target} has no add_executable()")
        block = cmake[start : m.start()]
        out[target] = set(TARGET_FILE_RE.findall(block)) - {target}
    if not out:
        raise ParseError("no LABELS concurrency targets found in CMakeLists.txt")
    # A label written in a form LABEL_RE cannot read (a quoted list, set_tests_properties, ...) would be
    # invisible to this guard - the very failure it exists to prevent. Refuse rather than under-count.
    mentions = [
        ln
        for ln in cmake.splitlines()
        if not ln.lstrip().startswith("#") and re.search(r"\bLABELS\b[^#]*\bconcurrency\b", ln)
    ]
    if len(mentions) != len(out):
        raise ParseError(
            f"{len(mentions)} lines carry a concurrency label but {len(out)} targets were parsed; "
            "teach LABEL_RE the new form"
        )
    return out


def declared_targets(cmake: str) -> set[str]:
    return set(re.findall(r"add_executable\(\s*([A-Za-z0-9_]+)", cmake))


def _is_comment(line: str) -> bool:
    return line.lstrip().startswith("#")


def _target_option_at(text: str) -> int:
    """Offset just past the first `--target` that sits in a command line, not in a comment."""
    pos = 0
    for line in text.split("\n"):
        if not _is_comment(line) and "--target" in line:
            return pos + line.index("--target") + len("--target")
        pos += len(line) + 1
    raise ParseError("no `--target` option outside a comment")


def _targets_after(text: str, what: str) -> set[str]:
    """Names listed after the first command-line `--target`, following backslash continuations."""
    try:
        start = _target_option_at(text)
    except ParseError as exc:
        raise ParseError(f"{what}: {exc}") from exc
    names: set[str] = set()
    for line in text[start:].split("\n"):
        stripped = line.strip()
        names.update(re.findall(r"[A-Za-z0-9_]+", stripped.rstrip("\\")))
        if not stripped.endswith("\\"):
            break
    return names


def _job_text(workflow: str, job: str) -> str:
    m = re.search(rf"^  {re.escape(job)}:\n(.*?)(?=^  [A-Za-z0-9_-]+:\n|\Z)", workflow, re.S | re.M)
    if not m:
        raise ParseError(f"job {job!r} not found in the sanitizer workflow")
    return m.group(1)


def workflow_lists(workflow: str) -> tuple[set[str], set[str]]:
    tsan = _targets_after(_job_text(workflow, TSAN_JOB), f"{TSAN_JOB} build step")
    arm = _targets_after(_job_text(workflow, ARM_JOB), f"{ARM_JOB} build step")
    return tsan, arm


def _run_thread_body(script: str) -> str:
    m = re.search(r"^run_thread\(\) \{\n(.*?)^\}", script, re.S | re.M)
    if not m:
        raise ParseError("run_thread() not found in tools/wsl_verify.sh")
    return m.group(1)


def wsl_list(script: str) -> set[str]:
    return _targets_after(_run_thread_body(script), "wsl_verify.sh run_thread")


def check(
    cmake: str,
    workflow: str,
    script: str,
    tsan_exempt: dict[str, str] | None = None,
    arm_exempt: dict[str, str] | None = None,
) -> list[Finding]:
    tsan_exempt = TSAN_EXEMPT if tsan_exempt is None else tsan_exempt
    arm_exempt = ARM64_EXEMPT if arm_exempt is None else arm_exempt
    labelled = labelled_targets(cmake)
    declared = declared_targets(cmake)
    tsan, arm = workflow_lists(workflow)
    wsl = wsl_list(script)
    out: list[Finding] = []

    for name, jobs, listed, exempt in (
        ("TSan", "TSAN", tsan, tsan_exempt),
        ("ARM64", "ARM", arm, arm_exempt),
    ):
        for target, helpers in sorted(labelled.items()):
            if target in listed:
                if target in exempt:
                    out.append(
                        Finding(
                            f"E-{jobs}-EXEMPT-STALE", target, f"is built by the {name} job and also exempt"
                        )
                    )
                for h in sorted(helpers - listed):
                    out.append(
                        Finding("E-HELPER-MISSING", target, f"the {name} job does not build its helper {h}")
                    )
            elif target not in exempt:
                out.append(
                    Finding(
                        f"E-{jobs}-MISSING",
                        target,
                        f"carries LABELS concurrency but the {name} job never builds it (add it to --target, "
                        "or exempt it with a reason)",
                    )
                )
        for target, reason in exempt.items():
            if target not in labelled:
                out.append(
                    Finding(f"E-{jobs}-EXEMPT-STALE", target, "is exempt but not labelled concurrency")
                )
            if len(reason.strip()) < 20:
                out.append(Finding("E-EXEMPT-REASON", target, f"the {name} exemption has no real reason"))

    if wsl != tsan:
        only_wsl, only_ci = sorted(wsl - tsan), sorted(tsan - wsl)
        out.append(
            Finding(
                "E-WSL-DRIFT",
                "tools/wsl_verify.sh",
                f"thread tier differs from the TSan job (only local: {only_wsl}; only CI: {only_ci})",
            )
        )
    for label, names in (("TSan job", tsan), ("ARM64 job", arm), ("wsl_verify.sh", wsl)):
        for n in sorted(names - declared):
            out.append(
                Finding("E-UNKNOWN-TARGET", n, f"is listed in the {label} but not declared in CMakeLists.txt")
            )
    return out


def read_all() -> tuple[str, str, str]:
    try:
        return (
            CMAKE.read_text(encoding="utf-8"),
            WORKFLOW.read_text(encoding="utf-8"),
            WSL_VERIFY.read_text(encoding="utf-8"),
        )
    except OSError as exc:
        raise ParseError(str(exc)) from exc


def _edit_list(text: str, old: str, new: str) -> str:
    """Replace the first `old` that follows the command-line `--target` (never one in a comment)."""
    cut = _target_option_at(text)
    if old not in text[cut:]:
        raise AssertionError(f"{old!r} is not in the target list")
    return text[:cut] + text[cut:].replace(old, new, 1)


def _edit_job_list(workflow: str, job: str, old: str, new: str) -> str:
    body = _job_text(workflow, job)
    return workflow.replace(body, _edit_list(body, old, new))


def self_test() -> int:
    cmake, workflow, script = read_all()
    results: list[tuple[str, bool, str]] = []

    def control(name: str, expect: str, c: str = cmake, w: str = workflow, s: str = script, **kw) -> None:
        try:
            codes = {f.code for f in check(c, w, s, **kw)}
        except ParseError as exc:
            codes = {"PARSE:" + str(exc)}
        ok = any(code == expect or code.startswith(expect) for code in codes)
        results.append((name, ok, f"没有报 {expect}，实际 {sorted(codes)[:4]}" if not ok else ""))

    def clean_control(name: str, c: str = cmake, w: str = workflow, s: str = script) -> None:
        try:
            found = [f.render() for f in check(c, w, s)]
        except ParseError as exc:
            found = ["PARSE:" + str(exc)]
        results.append((name, not found, "; ".join(found[:2])))

    clean_control("干净仓库：零发现")

    victim = "test_depth_feed_driver"
    control(
        "TSan 清单漏掉一个带标签的目标", "E-TSAN-MISSING", w=_edit_job_list(workflow, TSAN_JOB, victim, "")
    )

    control(
        "新增一个带标签却不在任何清单里的目标",
        "E-TSAN-MISSING",
        c=cmake
        + "\nadd_executable(test_brand_new_concurrency x.cpp)\n"
        + "gtest_discover_tests(test_brand_new_concurrency PROPERTIES LABELS concurrency)\n",
    )

    control(
        "ARM64 清单漏掉一个未豁免的目标",
        "E-ARM-MISSING",
        w=_edit_job_list(workflow, ARM_JOB, "test_kline_feed_driver", ""),
    )

    control(
        "TSan 作业不构建被测试拉起的辅助可执行文件",
        "E-HELPER-MISSING",
        w=_edit_job_list(workflow, TSAN_JOB, "compaction_lease_holder", ""),
    )
    control(
        "ARM64 加入了要拉起子进程的测试却没有它的辅助可执行文件",
        "E-HELPER-MISSING",
        w=_edit_job_list(
            workflow, ARM_JOB, "test_spsc_concurrency", "test_spsc_concurrency test_compaction_lease"
        ),
        arm_exempt={k: v for k, v in ARM64_EXEMPT.items() if k != "test_compaction_lease"},
    )

    control("豁免没有写理由", "E-EXEMPT-REASON", arm_exempt={**ARM64_EXEMPT, "test_compaction_lease": "n/a"})
    control(
        "豁免过期：目标其实已经在清单里",
        "E-ARM-EXEMPT-STALE",
        arm_exempt={
            **ARM64_EXEMPT,
            "test_spsc_concurrency": "a long enough but stale reason for the exemption",
        },
    )
    control(
        "豁免过期：目标根本没有带标签",
        "E-TSAN-EXEMPT-STALE",
        tsan_exempt={"test_not_labelled_at_all": "a long enough but stale reason for the exemption"},
    )
    control(
        "本地 thread 档与 CI 的 TSan 清单漂移",
        "E-WSL-DRIFT",
        s=_edit_list(script, "test_symbol_registry_concurrency", ""),
    )
    control(
        "清单里写了 CMake 里不存在的目标",
        "E-UNKNOWN-TARGET",
        w=_edit_job_list(workflow, TSAN_JOB, victim, "test_typo_target"),
    )
    control("工作流里找不到 TSan 作业", "PARSE:", w=workflow.replace(f"  {TSAN_JOB}:", "  renamed-job:"))
    control("CMake 里一个带标签的目标都没有", "PARSE:", c=cmake.replace("LABELS concurrency", "LABELS other"))
    control(
        "标签写成了本守卫读不懂的形式（引号列表）",
        "PARSE:",
        c=cmake
        + "\nadd_executable(test_quoted_label x.cpp)\n"
        + 'gtest_discover_tests(test_quoted_label PROPERTIES LABELS "slow;concurrency")\n',
    )
    # Regression: the first draft of this guard read a `--target` that sat in a comment as the list.
    clean_control(
        "注释里出现 --target 不被当成清单（脚本）",
        s=script.replace(
            "    python3 ../tools/concurrency_targets_check.py\n",
            "    # a --target mentioned in prose, list below\n"
            "    python3 ../tools/concurrency_targets_check.py\n",
            1,
        ),
    )
    clean_control(
        "注释里出现 --target 不被当成清单（工作流）",
        w=workflow.replace(
            "      - name: Build concurrency targets only\n",
            "      # prose mentioning --target, never built\n      - name: Build concurrency targets only\n",
        ),
    )

    width = max(len(n) for n, _, _ in results)
    for name, ok, why in results:
        print(f"{name:<{width}}  " + ("有效" if ok else f"无效（{why}）"))
    bad = [n for n, ok, _ in results if not ok]
    print(f"\n{len(results) - len(bad)}/{len(results)} 个对照有效。")
    return 1 if bad else 0


def main(argv: list[str] | None = None) -> int:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true", help="run the negative controls")
    ap.add_argument("--list", action="store_true", help="print the sets that were compared")
    ap.add_argument("--quiet", action="store_true", help="only report problems")
    args = ap.parse_args(argv)
    try:
        if args.self_test:
            return self_test()
        cmake, workflow, script = read_all()
        findings = check(cmake, workflow, script)
        if args.list:
            labelled = labelled_targets(cmake)
            tsan, arm = workflow_lists(workflow)
            print(f"labelled concurrency targets ({len(labelled)}): {sorted(labelled)}")
            print(f"TSan job builds ({len(tsan)}): {sorted(tsan)}")
            print(f"ARM64 job builds ({len(arm)}): {sorted(arm)}")
            print(f"wsl_verify.sh thread builds ({len(wsl_list(script))}): {sorted(wsl_list(script))}")
    except ParseError as exc:
        print(f"cannot parse: {exc}", file=sys.stderr)
        return 2
    for f in findings:
        print(f.render())
    if not args.quiet or findings:
        print(f"findings: {len(findings)}")
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
