"""winproc: ssh children never get a console window of their own (the scheduled task has no console)."""

from __future__ import annotations

import inspect
import os
import subprocess
import sys

import pytest

from hy_recorder import maint, pull, winproc


def test_hidden_kwargs_only_on_windows():
    assert winproc.hidden_window_kwargs("nt") == {"creationflags": 0x08000000}
    assert winproc.hidden_window_kwargs("posix") == {}


@pytest.mark.skipif(not hasattr(subprocess, "CREATE_NO_WINDOW"), reason="Windows only")
def test_flag_is_the_real_create_no_window():
    assert winproc._CREATE_NO_WINDOW == subprocess.CREATE_NO_WINDOW


@pytest.mark.parametrize(("name", "wrapper"), [("run", winproc.run_hidden), ("Popen", winproc.popen_hidden)])
def test_wrappers_add_the_flag_and_let_the_caller_override(monkeypatch, name, wrapper):
    seen: dict = {}

    def fake(*args, **kwargs):
        seen["args"], seen["kwargs"] = args, kwargs
        return "sentinel"

    monkeypatch.setattr(subprocess, name, fake)
    monkeypatch.setattr(winproc, "hidden_window_kwargs", lambda os_name=None: {"creationflags": 7})

    assert wrapper(["ssh", "h"], capture_output=True) == "sentinel"
    assert seen["args"] == (["ssh", "h"],)
    assert seen["kwargs"] == {"creationflags": 7, "capture_output": True}

    wrapper(["ssh"], creationflags=1)
    assert seen["kwargs"]["creationflags"] == 1


def test_every_local_ssh_spawn_site_defaults_to_the_hidden_variant():
    """A default reverted to ``subprocess.run``/``Popen`` brings the flashing windows back, silently."""
    params = inspect.signature(pull.SshTransport.__init__).parameters
    assert params["run"].default is winproc.run_hidden
    assert params["popen"].default is winproc.popen_hidden
    assert inspect.signature(maint.do_probe).parameters["run"].default is winproc.run_hidden


@pytest.mark.skipif(os.name != "nt", reason="console windows are a Windows concept")
def test_hidden_child_really_has_no_console_window():
    import ctypes

    argv = [sys.executable, "-c", "import ctypes; print(ctypes.windll.kernel32.GetConsoleWindow())"]
    hidden = winproc.run_hidden(argv, capture_output=True, text=True)
    assert hidden.stdout.strip() == "0"
    if ctypes.windll.kernel32.GetConsoleWindow():  # the contrast is only observable under a real console
        plain = subprocess.run(argv, capture_output=True, text=True)
        assert plain.stdout.strip() != "0"
