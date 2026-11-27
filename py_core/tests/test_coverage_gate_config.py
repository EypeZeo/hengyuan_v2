"""Audit P2-007: regression guard for the py_core coverage gate's own configuration.

The gate is three separate pieces that must stay together: ``ci-python.yml`` passes ``--cov`` to pytest,
``pyproject.toml`` carries the measurement settings and ``fail_under``, and ``requirements.txt`` pins
``coverage`` and ``pytest-cov`` with hashes. Removing or weakening any one of them leaves a green CI that no longer
measures anything, which is the failure this file exists to catch. It reads the three files as text and TOML; it does
not measure coverage itself (the CI step does).
"""

from __future__ import annotations

import re
import tomllib
from pathlib import Path

_PY_CORE = Path(__file__).resolve().parents[1]
_REPO_ROOT = _PY_CORE.parent

# The threshold the owner set on 2026-11-25. Raising `fail_under` is fine; lowering it needs this number changed in the
# same PR, on purpose.
_FLOOR = 90


def _coverage_config() -> dict:
    with (_PY_CORE / "pyproject.toml").open("rb") as fh:
        return tomllib.load(fh)["tool"]["coverage"]


def test_fail_under_is_not_below_the_floor_the_owner_set() -> None:
    assert _coverage_config()["report"]["fail_under"] >= _FLOOR


def test_branch_coverage_over_the_whole_directory_is_on() -> None:
    run = _coverage_config()["run"]
    assert run["branch"] is True
    # "." counts a library module that no test imports as 0% instead of leaving it out.
    assert run["source"] == ["."]


def test_only_tests_tools_and_environment_directories_are_omitted() -> None:
    omit = set(_coverage_config()["run"]["omit"])
    assert {"tests/*", "tools/*"} <= omit
    # Anything beyond tests, the experiment scripts and build/venv noise would be library code leaving the gate.
    assert omit <= {"tests/*", "tools/*", ".venv/*", "*.egg-info/*"}


def test_ci_python_workflow_runs_pytest_with_cov() -> None:
    workflow = (_REPO_ROOT / ".github" / "workflows" / "ci-python.yml").read_text(encoding="utf-8")
    runs = [line.strip() for line in workflow.splitlines() if re.match(r"\s*run:\s*python -m pytest\b", line)]
    assert runs, "ci-python.yml no longer runs pytest"
    assert all("--cov" in line for line in runs), runs
    # A flag on the command line can override the file's threshold downwards.
    assert not any("--cov-fail-under" in line or "--no-cov" in line for line in runs), runs


def test_requirements_pin_coverage_and_pytest_cov_with_hashes() -> None:
    lines = (_PY_CORE / "requirements.txt").read_text(encoding="utf-8").splitlines()
    for name in ("coverage", "pytest-cov"):
        index = next((i for i, line in enumerate(lines) if re.match(rf"{re.escape(name)}==\S+ \\$", line)), None)
        assert index is not None, f"{name} is not pinned with an exact version in requirements.txt"
        assert lines[index + 1].startswith("    --hash=sha256:"), f"{name} has no sha256 hash"
