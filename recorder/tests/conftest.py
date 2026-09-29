"""Shared test helpers: real-sample fixtures captured on 2026-09-29 (see DATA_DICTIONARY.md)."""

from __future__ import annotations

import threading
from pathlib import Path

import pytest

FIXTURES = Path(__file__).parent / "fixtures"
_MARK = b',"p":'


def load_frames(name: str) -> list[tuple[int, str, bytes]]:
    """Read a probe fixture. Probe lines look like ``{"r":<recv_us>,"s":"<label>","p":<frame>}`` with
    the frame spliced in exactly as received; returns ``(recv_us, label, exact_frame_bytes)``."""
    out = []
    for line in (FIXTURES / name).read_bytes().splitlines():
        if not line.strip():
            continue
        idx = line.index(_MARK)
        assert line.endswith(b"}")
        payload = line[idx + len(_MARK) : -1]
        head = line[:idx]
        recv_us = int(head.split(b'"r":')[1].split(b",")[0])
        label = head.split(b'"s":"')[1].split(b'"')[0].decode()
        out.append((recv_us, label, payload))
    return out


@pytest.fixture(scope="session")
def fixtures_dir() -> Path:
    return FIXTURES


@pytest.fixture(autouse=True)
def _stop_stray_writer_threads():
    """A failing test must not leave a running writer thread behind: it is not a daemon, so it would keep the
    interpreter from exiting (a CI job would sit there until its timeout instead of reporting the failure)."""
    yield
    for t in threading.enumerate():
        if t.name == "hy-writer" and t.is_alive():
            t.request_stop()  # type: ignore[attr-defined]
            t.join(10)
