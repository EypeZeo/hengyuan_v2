"""Unit tests for py_core.market_data._partition_lock (批次 1 PR-1).

The single-process tests exercise acquire/release/busy/exception-mapping. The cross-process
tests use a real spawned OS process (multiprocessing.get_context("spawn")) rather than two
handles in the same process -- flock/msvcrt ownership semantics for same-process multiple
file descriptors are not a reliable stand-in for real cross-process contention (this was a
review finding: same-process double-open tests do not exercise the same code path a second
real writer process would).
"""

from __future__ import annotations

import multiprocessing
import os
import time
from pathlib import Path

import pytest

from py_core.market_data._partition_lock import PartitionLockBusyError, partition_lock

_SPAWN_TIMEOUT_SECONDS = 20


def test_acquire_and_release_allows_reacquire(tmp_path: Path) -> None:
    lock_path = tmp_path / "p.parquet.lock"
    with partition_lock(lock_path):
        pass
    with partition_lock(lock_path):
        pass  # must not raise -- release from the first `with` must have actually happened


def test_second_attempt_while_held_raises_busy(tmp_path: Path) -> None:
    lock_path = tmp_path / "p.parquet.lock"
    with partition_lock(lock_path), pytest.raises(PartitionLockBusyError), partition_lock(lock_path):
        pass  # pragma: no cover -- must not be reached


def test_lock_file_gets_sentinel_byte(tmp_path: Path) -> None:
    lock_path = tmp_path / "p.parquet.lock"
    with partition_lock(lock_path):
        pass
    assert lock_path.exists()
    assert lock_path.stat().st_size >= 1


def test_reentrant_lock_within_same_call_stack_does_not_deadlock_silently(tmp_path: Path) -> None:
    # Not a supported use case, but confirms the busy error surfaces rather than hanging --
    # this module never blocks waiting for a lock it cannot get.
    lock_path = tmp_path / "p.parquet.lock"
    start = time.monotonic()
    with partition_lock(lock_path), pytest.raises(PartitionLockBusyError), partition_lock(lock_path):
        pass
    elapsed = time.monotonic() - start
    assert elapsed < 2.0, "must fail immediately, not after any internal wait"


def test_unrelated_oserror_is_not_reclassified_as_busy(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """An OSError that is not the lock-contention errno must propagate unchanged, not be
    swallowed into a generic Busy classification."""

    lock_path = tmp_path / "p.parquet.lock"

    if os.name == "posix":
        import fcntl

        def broken_flock(fd: int, op: int) -> None:
            raise OSError(28, "No space left on device")  # ENOSPC, unrelated to contention

        monkeypatch.setattr(fcntl, "flock", broken_flock)
    else:
        import msvcrt

        def broken_locking(fd: int, mode: int, nbytes: int) -> None:
            raise OSError(13, "Permission denied")  # deliberately not EACCES's usual meaning here

        # Force a non-EACCES errno by raising a plain OSError with an unrelated errno.
        def broken_locking2(fd: int, mode: int, nbytes: int) -> None:
            err = OSError()
            err.errno = 28  # ENOSPC-equivalent, not EACCES
            raise err

        monkeypatch.setattr(msvcrt, "locking", broken_locking2)

    with pytest.raises(OSError) as exc_info, partition_lock(lock_path):
        pass  # pragma: no cover
    assert not isinstance(exc_info.value, PartitionLockBusyError)


# ---------------------------------------------------------------------------
# Cross-process tests -- real spawn, not same-process double-open
# ---------------------------------------------------------------------------


def _child_hold_lock(lock_path_str: str, holding_evt, release_evt) -> None:
    """Runs in a spawned child process: acquire the lock, signal, wait to be told to
    release, then release."""
    from py_core.market_data._partition_lock import partition_lock as _lock

    with _lock(Path(lock_path_str)):
        holding_evt.set()
        release_evt.wait(timeout=_SPAWN_TIMEOUT_SECONDS)


@pytest.mark.skipif(
    multiprocessing.get_start_method(allow_none=True) not in (None, "spawn")
    and "spawn" not in multiprocessing.get_all_start_methods(),
    reason="spawn start method unavailable on this platform",
)
def test_cross_process_busy_then_release_then_retry_succeeds(tmp_path: Path) -> None:
    lock_path = tmp_path / "p.parquet.lock"
    ctx = multiprocessing.get_context("spawn")
    holding_evt = ctx.Event()
    release_evt = ctx.Event()
    proc = ctx.Process(target=_child_hold_lock, args=(str(lock_path), holding_evt, release_evt))
    proc.start()
    try:
        assert holding_evt.wait(timeout=_SPAWN_TIMEOUT_SECONDS), "child never signaled it holds the lock"

        # Parent, acting as a second real writer, must get Busy immediately -- non-blocking,
        # not "eventually succeeds after some internal wait".
        start = time.monotonic()
        with pytest.raises(PartitionLockBusyError), partition_lock(lock_path):
            pass  # pragma: no cover
        elapsed = time.monotonic() - start
        assert elapsed < 2.0, "parent must fail immediately while child holds the lock"

        # Tell the child to release, then the parent explicitly retries (no library-internal
        # retry -- this is the caller doing it).
        release_evt.set()
        proc.join(timeout=_SPAWN_TIMEOUT_SECONDS)
        assert proc.exitcode == 0

        with partition_lock(lock_path):
            pass  # must succeed now that the child released
    finally:
        if proc.is_alive():
            proc.terminate()
            proc.join(timeout=5)
