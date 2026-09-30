"""Recorder configuration. The connection layout is derived from the symbol list, never free-form.

Only numbers, the symbol list and the data root can be overridden from TOML; unknown keys are
rejected. URLs are always built by :mod:`hy_recorder.guard`, so a config file cannot point the
recorder at an arbitrary host or path.
"""

from __future__ import annotations

import hashlib
import re
import tomllib
from dataclasses import dataclass, field, replace
from pathlib import Path
from typing import Any

from .guard import build_ws_url


class ConfigError(ValueError):
    pass


@dataclass(frozen=True)
class StreamSpec:
    name: str  # e.g. "btcusdt@depth@100ms" or "!forceOrder@arr"
    kind: str  # depth | trade | aggTrade | markPrice | forceOrder
    symbol: str | None  # upper-case symbol, None for all-market streams
    max_gap_s: float | None  # longest tolerated silence; None = no expectation
    first_frame_s: float | None  # deadline for the first frame after subscribing; None = none


@dataclass(frozen=True)
class ConnectionSpec:
    name: str
    venue: str  # spot | usdm
    cls: str  # segment class: depth | trade | market
    url: str
    streams: tuple[StreamSpec, ...]

    def stream_id(self, stream_name: str) -> str:
        return "%s:%s" % (self.venue, stream_name)

    @property
    def stream_ids(self) -> tuple[str, ...]:
        return tuple(self.stream_id(s.name) for s in self.streams)

    @property
    def is_depth(self) -> bool:
        return self.cls == "depth"


_OVERRIDABLE = {
    "root": str,
    "symbols": list,
    "zstd_level": int,
    "queue_capacity": int,
    "snapshot_limit": int,
    "keyframe_interval_s": (int, float),
    "snapshot_cooldown_s": (int, float),
    "rest_budget_fraction": (int, float),
    "rest_pressure_fraction": (int, float),
    "time_probe_interval_s": (int, float),
    "exchange_info_interval_s": (int, float),
    "retain_hours": (int, float),
    "disk_warn_gb": (int, float),
    "disk_floor_gb": (int, float),
    "reserve_mb": int,
    "rotation_age_s": (int, float),
}
_SYMBOL_RE = re.compile(r"^[A-Z0-9]{5,20}$")


@dataclass(frozen=True)
class RecorderConfig:
    root: Path
    symbols: tuple[str, ...] = ("BTCUSDT",)
    zstd_level: int = 9
    queue_capacity: int = 20_000
    snapshot_limit: int = 1000
    keyframe_interval_s: float = 3600.0
    snapshot_cooldown_s: float = 5.0
    rest_budget_fraction: float = 0.05
    rest_pressure_fraction: float = 0.5
    time_probe_interval_s: float = 60.0
    exchange_info_interval_s: float = 86_400.0
    retain_hours: float = 48.0
    disk_warn_gb: float = 5.0
    disk_floor_gb: float = 2.0
    reserve_mb: int = 256
    rotation_age_s: float = 23.5 * 3600
    connections: tuple[ConnectionSpec, ...] = field(default=())

    def validate(self) -> RecorderConfig:
        if not 1 <= self.zstd_level <= 19:
            raise ConfigError("zstd_level must be 1..19")
        if not self.symbols or len(self.symbols) > 10:
            raise ConfigError("symbols must have 1..10 entries")
        for s in self.symbols:
            if not _SYMBOL_RE.match(s):
                raise ConfigError("bad symbol %r" % s)
        if self.snapshot_limit not in (100, 500, 1000):
            raise ConfigError("snapshot_limit must be 100, 500 or 1000")
        if self.snapshot_cooldown_s < 1.0:
            raise ConfigError("snapshot_cooldown_s must be >= 1")
        if not 0 < self.rest_budget_fraction <= 0.2:
            raise ConfigError("rest_budget_fraction must be in (0, 0.2]")
        if not 0.1 <= self.rest_pressure_fraction <= 0.9:
            raise ConfigError("rest_pressure_fraction must be in [0.1, 0.9]")
        if self.retain_hours < 6:
            raise ConfigError("retain_hours must be >= 6")
        if not 0 < self.disk_floor_gb < self.disk_warn_gb:
            raise ConfigError("need 0 < disk_floor_gb < disk_warn_gb")
        if self.queue_capacity < 100:
            raise ConfigError("queue_capacity must be >= 100")
        if self.rotation_age_s < 3600 or self.rotation_age_s > 23.9 * 3600:
            raise ConfigError("rotation_age_s must be between 1 h and 23.9 h")
        return self

    @property
    def depth_targets(self) -> tuple[tuple[str, str], ...]:
        """``(venue, SYMBOL)`` pairs that need snapshots."""
        return tuple(
            (c.venue, s.symbol) for c in self.connections if c.is_depth for s in c.streams if s.symbol
        )

    def fingerprint(self) -> str:
        """Stable hash of everything that shapes the recording (recorded in PROC_START)."""
        blob = repr(
            (
                self.symbols,
                self.zstd_level,
                self.queue_capacity,
                self.snapshot_limit,
                self.keyframe_interval_s,
                self.snapshot_cooldown_s,
                self.rest_budget_fraction,
                self.retain_hours,
                [(c.name, c.url) for c in self.connections],
            )
        )
        return hashlib.sha256(blob.encode()).hexdigest()[:16]


def build_connections(symbols: tuple[str, ...]) -> tuple[ConnectionSpec, ...]:
    """The S1 layout: one depth connection per (venue, symbol) so a depth gap only reconnects that
    connection; one spot trade connection; one USD-M market connection (aggTrade, markPrice,
    all-market forceOrder) on the ``/market`` route. Depth uses ``/public`` on USD-M."""
    conns: list[ConnectionSpec] = []
    for sym in symbols:
        low = sym.lower()
        spot_depth = StreamSpec("%s@depth@100ms" % low, "depth", sym, 5.0, 20.0)
        usdm_depth = StreamSpec("%s@depth@100ms" % low, "depth", sym, 5.0, 20.0)
        conns.append(
            ConnectionSpec(
                "spot_depth_%s" % low, "spot", "depth", build_ws_url("spot", [spot_depth.name]), (spot_depth,)
            )
        )
        conns.append(
            ConnectionSpec(
                "usdm_depth_%s" % low,
                "usdm",
                "depth",
                build_ws_url("usdm_public", [usdm_depth.name]),
                (usdm_depth,),
            )
        )
    trades = tuple(StreamSpec("%s@trade" % s.lower(), "trade", s, 30.0, 30.0) for s in symbols)
    conns.append(
        ConnectionSpec("spot_trade", "spot", "trade", build_ws_url("spot", [t.name for t in trades]), trades)
    )
    market: list[StreamSpec] = []
    for s in symbols:
        low = s.lower()
        market.append(StreamSpec("%s@aggTrade" % low, "aggTrade", s, 30.0, 30.0))
        market.append(StreamSpec("%s@markPrice@1s" % low, "markPrice", s, 5.0, 20.0))
    market.append(StreamSpec("!forceOrder@arr", "forceOrder", None, None, None))
    conns.append(
        ConnectionSpec(
            "usdm_market",
            "usdm",
            "market",
            build_ws_url("usdm_market", [m.name for m in market]),
            tuple(market),
        )
    )
    return tuple(conns)


def default_config(
    root: Path | str, symbols: tuple[str, ...] = ("BTCUSDT",), **overrides: Any
) -> RecorderConfig:
    cfg = RecorderConfig(root=Path(root), symbols=tuple(symbols), **overrides).validate()
    return replace(cfg, connections=build_connections(cfg.symbols))


def load_config(path: Path | str) -> RecorderConfig:
    """Load a TOML file with a single ``[recorder]`` table."""
    with open(path, "rb") as fh:
        data = tomllib.load(fh)
    unknown_tables = set(data) - {"recorder"}
    if unknown_tables:
        raise ConfigError("unknown table(s): %s" % sorted(unknown_tables))
    table = data.get("recorder", {})
    unknown = set(table) - set(_OVERRIDABLE)
    if unknown:
        raise ConfigError("unknown key(s): %s" % sorted(unknown))
    for key, value in table.items():
        want = _OVERRIDABLE[key]
        if isinstance(value, bool) or not isinstance(value, want):
            raise ConfigError("%s has the wrong type" % key)
    if "root" not in table:
        raise ConfigError("root is required")
    root = table.pop("root")
    symbols = tuple(str(s).upper() for s in table.pop("symbols", ["BTCUSDT"]))
    return default_config(root, symbols, **table)
