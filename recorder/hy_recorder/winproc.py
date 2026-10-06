"""Child processes that do not pop up a console window on Windows.

The scheduled maintenance task runs under ``pythonw.exe``, which has no console. A console-subsystem child
(``ssh.exe``) started from a process without a console gets a *new* console window of its own, so one pull
-- one ``ssh`` per segment, hundreds of them -- shows up as hundreds of black windows flashing open and shut.
``CREATE_NO_WINDOW`` gives the child a console that has no window instead. Everywhere else this is a no-op.
"""

from __future__ import annotations

import os
import subprocess
from typing import Any

_CREATE_NO_WINDOW = 0x08000000  # ``subprocess.CREATE_NO_WINDOW`` exists on Windows only


def hidden_window_kwargs(os_name: str | None = None) -> dict[str, int]:
    """The ``subprocess`` keyword arguments that suppress a child's console window (empty off Windows)."""
    return {"creationflags": _CREATE_NO_WINDOW} if (os.name if os_name is None else os_name) == "nt" else {}


def run_hidden(*args: Any, **kwargs: Any) -> subprocess.CompletedProcess[Any]:
    """``subprocess.run`` that never opens a console window; an explicit ``creationflags`` still wins."""
    return subprocess.run(*args, **{**hidden_window_kwargs(), **kwargs})


def popen_hidden(*args: Any, **kwargs: Any) -> subprocess.Popen[Any]:
    """``subprocess.Popen`` that never opens a console window; an explicit ``creationflags`` still wins."""
    return subprocess.Popen(*args, **{**hidden_window_kwargs(), **kwargs})
