"""Record envelope: one standard JSON object per line, the message text kept as a JSON string.

Line layout (``\\n`` terminated, keys in this fixed order)::

    {"v":1,"k":"m","r":<run>,"q":<seq>,"g":<gen>,"t":<wall_us>,"m":<mono_us>,
     "s":"<stream>","p":"<message text>"}

* ``k``  ``m`` = WebSocket message, ``s`` = REST snapshot body.
* ``t``  host wall clock, epoch microseconds; ``m`` host monotonic clock, microseconds.
  Integers only, and microseconds keep them below 2**53 for decades (no float64 precision trap).
* ``p``  the message text exactly as delivered by the WebSocket library, stored as a JSON string.
  ``json.loads(line)["p"]`` returns the identical text (JSON string encoding is lossless), so
  the exact payload bytes are ``text.encode("utf-8")``. There is no custom extraction rule.
* Binary messages carry ``"b"`` (base64) and ``"x"`` (``"binary"``) instead of ``"p"``.

Why a string and not the nested object: a nested payload made Polars fail on mixed-stream files
(struct supertype error) and DuckDB fail on the ``e`` / ``E`` key pair (case-insensitive struct
names). With a string payload pandas, Polars and DuckDB all read the file directly, at a measured
cost of about +1.4 % compressed size (real frames, zstd level 9).

The guarantee is **application-layer message payload fidelity**: this is what the WebSocket
library hands to the application after frame reassembly, not the wire frames. Control frames are
not recorded.
"""

from __future__ import annotations

import base64
import json
import re
from dataclasses import dataclass
from typing import Any

SCHEMA_VERSION = 1
KIND_MESSAGE = "m"
KIND_SNAPSHOT = "s"
_KINDS = (KIND_MESSAGE, KIND_SNAPSHOT)
_STREAM_RE = re.compile(r"^[A-Za-z0-9_.:@!\-]{1,96}$")


class EnvelopeError(ValueError):
    """Malformed envelope or invalid argument."""


def _json_string(text: str) -> bytes:
    """Encode ``text`` as a JSON string. U+2028 / U+2029 are escaped as well so that readers that
    split lines on Unicode separators (``str.splitlines``) cannot break a record apart."""
    s = json.dumps(text, ensure_ascii=False)
    return s.replace("\u2028", "\\u2028").replace("\u2029", "\\u2029").encode("utf-8")


def encode_record(
    *,
    run: int,
    seq: int,
    gen: int,
    wall_us: int,
    mono_us: int,
    stream: str,
    payload: str | bytes,
    kind: str = KIND_MESSAGE,
    binary: bool = False,
) -> tuple[bytes, str | None]:
    """Build one record line. Returns ``(line, fallback_reason)``.

    A ``str`` payload (a WebSocket text message) is stored as a JSON string. Bytes are stored as
    text only if they are valid UTF-8 and ``binary`` is false; otherwise they are base64-encoded
    with reason ``"binary"`` or ``"invalid_utf8"``.
    """
    if kind not in _KINDS:
        raise EnvelopeError("bad kind %r" % kind)
    if not _STREAM_RE.match(stream):
        raise EnvelopeError("bad stream id %r" % stream)
    for name, value in (("run", run), ("seq", seq), ("gen", gen), ("wall_us", wall_us), ("mono_us", mono_us)):
        if not isinstance(value, int) or isinstance(value, bool) or value < 0:
            raise EnvelopeError("%s must be a non-negative int" % name)
    head = b'{"v":%d,"k":"%s","r":%d,"q":%d,"g":%d,"t":%d,"m":%d,"s":"%s",' % (
        SCHEMA_VERSION,
        kind.encode("ascii"),
        run,
        seq,
        gen,
        wall_us,
        mono_us,
        stream.encode("ascii"),
    )

    def as_base64(raw: bytes, reason: str) -> tuple[bytes, str]:
        return head + b'"b":"' + base64.b64encode(raw) + b'","x":"' + reason.encode("ascii") + b'"}\n', reason

    if binary:
        return as_base64(
            payload if isinstance(payload, bytes) else payload.encode("utf-8", "surrogatepass"), "binary"
        )
    if isinstance(payload, bytes):
        try:
            text = payload.decode("utf-8")
        except UnicodeDecodeError:
            return as_base64(payload, "invalid_utf8")
    else:
        text = payload
    try:
        body = _json_string(text)
    except UnicodeEncodeError:  # a lone surrogate cannot be written as UTF-8 text
        return as_base64(text.encode("utf-8", "surrogatepass"), "invalid_utf8")
    return head + b'"p":' + body + b"}\n", None


@dataclass(frozen=True)
class Record:
    """A decoded record line."""

    run: int
    seq: int
    gen: int
    wall_us: int
    mono_us: int
    stream: str
    kind: str
    payload: bytes  # exact payload bytes (UTF-8 of the message text, or the raw binary message)
    data: Any  # the payload parsed as JSON; ``None`` if it is binary or not valid JSON
    fallback_reason: str | None  # set for base64 records

    @property
    def order(self) -> tuple[int, int]:
        return (self.run, self.seq)


def decode_record(line: bytes) -> Record:
    """Decode one record line; raises :class:`EnvelopeError` if it is malformed."""
    try:
        obj = json.loads(line)
    except ValueError as exc:
        raise EnvelopeError("line is not JSON: %s" % exc) from exc
    if not isinstance(obj, dict) or obj.get("v") != SCHEMA_VERSION:
        raise EnvelopeError("unsupported envelope")
    try:
        run, seq, gen = int(obj["r"]), int(obj["q"]), int(obj["g"])
        wall_us, mono_us = int(obj["t"]), int(obj["m"])
        stream, kind = str(obj["s"]), str(obj["k"])
    except (KeyError, TypeError, ValueError) as exc:
        raise EnvelopeError("missing or bad field: %s" % exc) from exc
    if kind not in _KINDS or not _STREAM_RE.match(stream):
        raise EnvelopeError("bad kind or stream")
    if "p" in obj:
        text = obj["p"]
        if not isinstance(text, str):
            raise EnvelopeError("payload must be a JSON string")
        payload = text.encode("utf-8")
        try:
            data = json.loads(text)
        except ValueError:
            data = None
        return Record(run, seq, gen, wall_us, mono_us, stream, kind, payload, data, None)
    if "b" in obj and "x" in obj:
        try:
            payload = base64.b64decode(obj["b"], validate=True)
        except (ValueError, TypeError) as exc:
            raise EnvelopeError("bad base64: %s" % exc) from exc
        return Record(run, seq, gen, wall_us, mono_us, stream, kind, payload, None, str(obj["x"]))
    raise EnvelopeError("record has neither p nor b/x")
