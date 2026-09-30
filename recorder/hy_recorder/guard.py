"""Structural read-only guard: the recorder can only reach a fixed allowlist of public endpoints.

Every outgoing request is built here from an enumerated ``(venue, endpoint)`` pair with a fixed set of
allowed query parameters and no custom headers, so even a bug in a caller cannot produce a private or
trading request: such a request simply cannot be constructed from this module.
"""

from __future__ import annotations

import re
from urllib.parse import urlencode

# -- REST ----------------------------------------------------------------------------------------
# (venue, endpoint) -> (host, path, allowed query parameters)
REST_ENDPOINTS: dict[tuple[str, str], tuple[str, str, frozenset[str]]] = {
    ("spot", "depth"): ("api.binance.com", "/api/v3/depth", frozenset({"symbol", "limit"})),
    ("spot", "time"): ("api.binance.com", "/api/v3/time", frozenset()),
    ("spot", "exchangeInfo"): ("api.binance.com", "/api/v3/exchangeInfo", frozenset({"symbol"})),
    ("usdm", "depth"): ("fapi.binance.com", "/fapi/v1/depth", frozenset({"symbol", "limit"})),
    ("usdm", "time"): ("fapi.binance.com", "/fapi/v1/time", frozenset()),
    ("usdm", "exchangeInfo"): ("fapi.binance.com", "/fapi/v1/exchangeInfo", frozenset()),
}

_SYMBOL_RE = re.compile(r"^[A-Z0-9]{5,20}$")
_LIMITS = frozenset({5, 10, 20, 50, 100, 500, 1000, 5000})

# Request weight per depth limit (verified against real responses on 2026-09-29).
_SPOT_DEPTH_WEIGHT = ((100, 5), (500, 25), (1000, 50), (5000, 250))
_USDM_DEPTH_WEIGHT = ((50, 2), (100, 5), (500, 10), (1000, 20))

REST_WEIGHT_LIMIT_PER_MIN = {"spot": 6000, "usdm": 2400}


class ForbiddenEndpoint(ValueError):
    """The requested endpoint or parameter is not on the read-only allowlist."""


def depth_weight(venue: str, limit: int) -> int:
    table = _SPOT_DEPTH_WEIGHT if venue == "spot" else _USDM_DEPTH_WEIGHT
    for upto, weight in table:
        if limit <= upto:
            return weight
    raise ForbiddenEndpoint("depth limit %r not allowed for %s" % (limit, venue))


def endpoint_weight(venue: str, endpoint: str, params: dict[str, object]) -> int:
    if endpoint == "depth":
        return depth_weight(venue, int(params["limit"]))
    if endpoint == "time":
        return 1
    if endpoint == "exchangeInfo":
        return 20 if venue == "spot" else 1
    raise ForbiddenEndpoint(endpoint)


def build_rest_request(venue: str, endpoint: str, params: dict[str, object] | None = None) -> tuple[str, str]:
    """Return ``(host, path_with_query)`` for an allowlisted GET, or raise :class:`ForbiddenEndpoint`."""
    key = (venue, endpoint)
    if key not in REST_ENDPOINTS:
        raise ForbiddenEndpoint("endpoint %r not allowed" % (key,))
    host, path, allowed = REST_ENDPOINTS[key]
    params = dict(params or {})
    extra = set(params) - allowed
    if extra:
        raise ForbiddenEndpoint("parameters %s not allowed for %s" % (sorted(extra), key))
    if "symbol" in params and not _SYMBOL_RE.match(str(params["symbol"])):
        raise ForbiddenEndpoint("bad symbol %r" % params["symbol"])
    if "limit" in params:
        if not isinstance(params["limit"], int) or params["limit"] not in _LIMITS:
            raise ForbiddenEndpoint("bad limit %r" % params["limit"])
    if endpoint == "depth" and ("symbol" not in params or "limit" not in params):
        raise ForbiddenEndpoint("depth needs symbol and limit")
    query = urlencode(sorted(params.items()))
    return host, path + ("?" + query if query else "")


# -- WebSocket -----------------------------------------------------------------------------------
# route name -> (host, port, path)
WS_ROUTES: dict[str, tuple[str, int, str]] = {
    "spot": ("stream.binance.com", 9443, "/stream"),
    "usdm_public": ("fstream.binance.com", 443, "/public/stream"),
    "usdm_market": ("fstream.binance.com", 443, "/market/stream"),
}
_STREAM_NAME_RE = re.compile(
    r"^(?:[a-z0-9]{3,20}@[A-Za-z0-9_]{2,30}(?:@[A-Za-z0-9]{1,10})?|![A-Za-z0-9]{2,20}@arr(?:@[A-Za-z0-9]{1,10})?)$"
)


def validate_stream_name(name: str) -> str:
    if not _STREAM_NAME_RE.match(name):
        raise ForbiddenEndpoint("bad stream name %r" % name)
    return name


def build_ws_url(route: str, streams: list[str]) -> str:
    if route not in WS_ROUTES:
        raise ForbiddenEndpoint("ws route %r not allowed" % route)
    if not streams or len(streams) > 20:
        raise ForbiddenEndpoint("need 1..20 streams")
    host, port, path = WS_ROUTES[route]
    names = "/".join(validate_stream_name(s) for s in streams)
    port_part = "" if port == 443 else ":%d" % port
    return "wss://%s%s%s?streams=%s" % (host, port_part, path, names)
