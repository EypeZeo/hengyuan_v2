"""Cross-platform single-file exclusive lock — 批次 1 PR-1 (warehouse.py's sole consumer).

``partition_lock()`` is the only entry point: it non-blockingly acquires an exclusive lock
on the ``[0, 1)`` byte range of a lock file. Failing to acquire raises
``PartitionLockBusyError`` immediately — this module never retries or blocks waiting. Retry
policy is entirely the caller's responsibility (``warehouse.py`` never retries internally
either; a human/CLI layer, not yet built, would own that decision in a later batch).

The lock's lifetime is bound to the *open file handle*, not to the lock file's mere
existence: if the holding process crashes, the OS releases the lock automatically when the
handle is closed by process teardown. A lock file that exists on disk is never itself
evidence that the lock is currently held.

Windows requires the lock file to be at least 1 byte long before ``msvcrt.locking()`` is
called on byte range ``[0, 1)`` — locking a 0-byte file behaves inconsistently across some
msvcrt/Windows version combinations. The first time a lock file is created, a single sentinel
byte is written and flushed before the lock is attempted; later opens see the file already
non-empty and skip that step.

This module does **not** create parent directories and does **not** validate the lock path
for path-traversal/symlink safety — both are ``warehouse.py``'s responsibility (it derives
the lock path from an already-validated partition path and re-checks containment after
creating the parent directory). Keeping that logic in one place avoids two independent,
possibly-drifting implementations of the same safety check.
"""

from __future__ import annotations

import errno
import os
import sys
from collections.abc import Iterator
from contextlib import contextmanager
from pathlib import Path

_SENTINEL_BYTE = b"\x00"


class PartitionLockError(Exception):
    """Base class for this module's own errors."""


class PartitionLockBusyError(PartitionLockError):
    """Another handle (this process or another) already holds the lock."""


if sys.platform == "win32":
    import msvcrt

    @contextmanager
    def partition_lock(lock_path: Path) -> Iterator[None]:
        """Non-blocking exclusive lock on ``lock_path``'s first byte (Windows/msvcrt)."""
        # O_CREAT without O_TRUNC: create if absent, never clear an existing sentinel byte.
        fd = os.open(str(lock_path), os.O_RDWR | os.O_CREAT | os.O_BINARY)
        acquired = False
        try:
            if os.fstat(fd).st_size == 0:
                os.write(fd, _SENTINEL_BYTE)
                os.fsync(fd)
            os.lseek(fd, 0, os.SEEK_SET)
            try:
                msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)
                acquired = True
            except OSError as exc:
                if exc.errno == errno.EACCES:
                    raise PartitionLockBusyError(f"partition lock busy: {lock_path}") from exc
                raise  # any other OSError (disk full, permission issue, ...) propagates as-is
            yield
        finally:
            if acquired:
                os.lseek(fd, 0, os.SEEK_SET)
                msvcrt.locking(fd, msvcrt.LK_UNLCK, 1)
            os.close(fd)

else:
    import fcntl

    @contextmanager
    def partition_lock(lock_path: Path) -> Iterator[None]:
        """Non-blocking exclusive lock on ``lock_path`` (POSIX/flock)."""
        fd = os.open(str(lock_path), os.O_RDWR | os.O_CREAT)
        acquired = False
        try:
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                acquired = True
            except OSError as exc:
                if exc.errno in (errno.EWOULDBLOCK, errno.EAGAIN):
                    raise PartitionLockBusyError(f"partition lock busy: {lock_path}") from exc
                raise
            yield
        finally:
            if acquired:
                fcntl.flock(fd, fcntl.LOCK_UN)
            os.close(fd)
