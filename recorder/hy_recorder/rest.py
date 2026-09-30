"""Blocking HTTPS GET client for the allowlisted public endpoints (keep-alive, one connection per host).

Call it from a worker thread (``asyncio.to_thread``); it never runs on the event loop. The request
line is built by :mod:`hy_recorder.guard`, so only allowlisted paths and parameters can be sent, and
the only headers are a user agent and ``Accept``.
"""

from __future__ import annotations

import http.client
import ssl
from collections.abc import Callable
from dataclasses import dataclass
from typing import Any

from . import __version__
from .clock import SYSTEM_CLOCK, Clock
from .guard import build_rest_request

MAX_BODY = 8 * 1024 * 1024
_USER_AGENT = "hy-recorder/%s (read-only public data)" % __version__


class RestError(Exception):
    """Transport-level failure (no HTTP response)."""

    def __init__(self, kind: str, detail: str = "") -> None:
        super().__init__("%s %s" % (kind, detail))
        self.kind = kind
        self.detail = detail


@dataclass(frozen=True)
class RestResult:
    status: int
    headers: dict[str, str]  # lower-case keys
    body: bytes
    start_mono_us: int
    end_mono_us: int
    start_wall_us: int


class RestClient:
    def __init__(
        self,
        *,
        timeout_s: float = 10.0,
        clock: Clock = SYSTEM_CLOCK,
        conn_factory: Callable[[str], Any] | None = None,
    ) -> None:
        self._timeout = timeout_s
        self._clock = clock
        self._factory = conn_factory or self._default_conn
        self._conns: dict[str, Any] = {}

    def _default_conn(self, host: str) -> http.client.HTTPSConnection:
        return http.client.HTTPSConnection(
            host, 443, timeout=self._timeout, context=ssl.create_default_context()
        )

    def _drop(self, host: str) -> None:
        conn = self._conns.pop(host, None)
        if conn is not None:
            try:
                conn.close()
            except OSError:
                pass

    def close(self) -> None:
        for host in list(self._conns):
            self._drop(host)

    def get(self, venue: str, endpoint: str, params: dict[str, object] | None = None) -> RestResult:
        host, path = build_rest_request(venue, endpoint, params)  # raises ForbiddenEndpoint
        for attempt in (1, 2):
            conn = self._conns.get(host)
            if conn is None:
                conn = self._conns[host] = self._factory(host)
            wall = self._clock.wall_us()
            t0 = self._clock.mono_us()
            try:
                conn.request("GET", path, headers={"User-Agent": _USER_AGENT, "Accept": "application/json"})
                resp = conn.getresponse()
                body = resp.read(MAX_BODY + 1)
            except (OSError, http.client.HTTPException) as exc:
                self._drop(host)
                stale = isinstance(
                    exc,
                    (
                        http.client.RemoteDisconnected,
                        ConnectionResetError,
                        BrokenPipeError,
                        http.client.CannotSendRequest,
                        http.client.BadStatusLine,
                    ),
                )
                if attempt == 1 and stale:
                    continue  # keep-alive connection closed by the server: retry once (GET is idempotent)
                raise RestError("transport", type(exc).__name__) from exc
            t1 = self._clock.mono_us()
            if len(body) > MAX_BODY:
                self._drop(host)
                raise RestError("too_large", str(len(body)))
            headers = {k.lower(): v for k, v in resp.getheaders()}
            if getattr(resp, "will_close", False) or headers.get("connection", "").lower() == "close":
                self._drop(host)
            return RestResult(resp.status, headers, body, t0, t1, wall)
        raise RestError("transport", "unreachable")  # pragma: no cover
