"""Streaming reader for sealed ``.jsonl.zst`` segments (single zstd frame expected)."""

from __future__ import annotations

from collections.abc import Iterator
from pathlib import Path

import zstandard as zstd

_CHUNK = 1 << 16


class SegmentReader:
    """Iterate the lines of one segment and record how the frame ended.

    After :meth:`lines` is exhausted: ``complete`` (frame end reached, checksum verified by the
    decoder), ``trailing`` (bytes after the frame end: multi-frame or garbage), ``partial_tail``
    (a last line without newline) and ``error`` (decoder error text) describe the file.
    """

    def __init__(self, path: Path) -> None:
        self.path = Path(path)
        self.complete = False
        self.trailing = False
        self.partial_tail = False
        self.error: str | None = None

    def lines(self) -> Iterator[bytes]:
        dobj = zstd.ZstdDecompressor().decompressobj()
        pending = b""
        with open(self.path, "rb") as fh:
            while True:
                chunk = fh.read(_CHUNK)
                if not chunk:
                    break
                try:
                    out = dobj.decompress(chunk)
                except zstd.ZstdError as exc:
                    self.error = "zstd: %s" % exc
                    break
                if out:
                    parts = (pending + out).split(b"\n")
                    pending = parts[-1]
                    yield from parts[:-1]
                if getattr(dobj, "eof", False):
                    self.complete = True
                    if dobj.unused_data or fh.read(1):
                        self.trailing = True
                    break
        if pending:
            self.partial_tail = True
